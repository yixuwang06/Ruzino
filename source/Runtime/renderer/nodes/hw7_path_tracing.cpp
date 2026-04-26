#include <pxr/base/gf/vec2i.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include "../source/renderTLAS.h"
#include "../source/internal/memory/DeviceMemoryPool.hpp"
#include "GPUContext/program_vars.hpp"
#include "GPUContext/raytracing_context.hpp"
#include "RHI/internal/resources.hpp"
#include "Scene/MaterialParamsBuffer.slang"
#include "camera.h"
#include "hd_RUZINO/render_node_base.h"
#include "light.h"
#include "material/material.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "shaders/shaders/utils/HitObject.h"

NODE_DEF_OPEN_SCOPE

struct HW7PathTracingStorage {
    constexpr static bool has_storage = false;
    pxr::GfVec2i old_size = pxr::GfVec2i(-1, -1);

    ProgramHandle program;
    std::unordered_map<unsigned, std::string> callable_shaders;
    std::unordered_map<unsigned, unsigned> custom_shader_eval_indices;
    ResourceAllocator* rc = nullptr;

    nvrhi::TextureHandle output;
    nvrhi::BufferHandle material_params_buffer;
    nvrhi::BufferHandle constants_buffer;
    nvrhi::SamplerHandle sampler;

    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<RaytracingContext> cached_rt_context;

    int cached_max_depth = -1;
    int cached_rr_start_depth = -1;
    float cached_rr_floor = -1.0f;

    ~HW7PathTracingStorage()
    {
        if (program && rc) {
            rc->destroy(program);
            program = nullptr;
        }
        if (material_params_buffer && rc) {
            rc->destroy(material_params_buffer);
            material_params_buffer = nullptr;
        }
        if (constants_buffer && rc) {
            rc->destroy(constants_buffer);
            constants_buffer = nullptr;
        }
        if (sampler && rc) {
            rc->destroy(sampler);
            sampler = nullptr;
        }
    }
};

NODE_DECLARATION_FUNCTION(hw7_path_tracing)
{
    b.add_input<nvrhi::BufferHandle>("Pixel Target");
    b.add_input<nvrhi::BufferHandle>("Rays");
    b.add_input<nvrhi::BufferHandle>("Random Seeds");
    b.add_input<int>("Max Depth").min(1).max(32).default_val(8);
    b.add_input<int>("RR Start Depth").min(1).max(16).default_val(3);
    b.add_input<float>("RR Floor").min(0.01f).max(1.0f).default_val(0.1f);

    b.add_output<nvrhi::TextureHandle>("Output");
}

struct HW7PathTracingConstants {
    uint32_t lightCount;
    uint32_t materialFetchCallableBaseIndex;
    uint32_t materialOpacityCallableOffset;
    uint32_t maxDepth;
    uint32_t rrStartDepth;
    float rrProbFloor;
};

NODE_EXECUTION_FUNCTION(hw7_path_tracing)
{
    using namespace nvrhi;

    constexpr int kMinDepth = 1;
    constexpr int kMaxDepth = 32;
    constexpr int kMinRRStartDepth = 1;
    constexpr int kMaxRRStartDepth = 16;
    constexpr float kMinRRFloor = 0.01f;
    constexpr float kMaxRRFloor = 1.0f;

    auto& g = global_payload;
    auto geom_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);
    auto mat_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyMaterials);
    auto light_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyLights);

    auto size = get_free_camera(params)->dataWindow.GetSize();
    auto& storage = params.get_storage<HW7PathTracingStorage&>();
    bool size_changed = (storage.old_size != size);
    storage.old_size = size;

#ifdef _DEBUG
    if (storage.program && storage.program->get_desc().check_shader_updated()) {
        mat_dirty = true;
    }
#endif

    if (geom_dirty || mat_dirty || light_dirty || size_changed) {
        spdlog::info(
            "HW7 path tracing: geom_dirty={}, mat_dirty={}, light_dirty={}, size_changed={}",
            geom_dirty,
            mat_dirty,
            light_dirty,
            size_changed);
    }

    storage.rc = &(resource_allocator);

    int requested_max_depth = params.get_input<int>("Max Depth");
    int requested_rr_start_depth = params.get_input<int>("RR Start Depth");
    float requested_rr_floor = params.get_input<float>("RR Floor");

    // UI input ranges are enforced interactively, but saved node graphs can
    // still carry stale or out-of-range values.
    int max_depth = std::clamp(requested_max_depth, kMinDepth, kMaxDepth);
    int rr_start_depth = std::clamp(
        requested_rr_start_depth,
        kMinRRStartDepth,
        std::min(kMaxRRStartDepth, max_depth));
    float rr_floor =
        std::clamp(requested_rr_floor, kMinRRFloor, kMaxRRFloor);

    if (requested_max_depth != max_depth ||
        requested_rr_start_depth != rr_start_depth ||
        requested_rr_floor != rr_floor) {
        spdlog::warn(
            "HW7 path tracing clamped settings: depth {}->{} rr_start {}->{} rr_floor {}->{}",
            requested_max_depth,
            max_depth,
            requested_rr_start_depth,
            rr_start_depth,
            requested_rr_floor,
            rr_floor);
    }

    bool settings_changed =
        storage.cached_max_depth != max_depth ||
        storage.cached_rr_start_depth != rr_start_depth ||
        storage.cached_rr_floor != rr_floor;

    if (mat_dirty || !storage.program) {
        ProgramDesc program_desc;
        program_desc.set_path("shaders/hw7_path_tracing.slang");
        program_desc.shaderType = nvrhi::ShaderType::AllRayTracing;
        program_desc.define("USE_RGB_SPECTRUM", "1");
        program_desc.add_path("shaders/callables/eval_fallback.slang");
        program_desc.add_path("shaders/callables/eval_standard_surface.slang");
        program_desc.add_path("shaders/callables/eval_preview_surface.slang");

        storage.callable_shaders.clear();
        storage.custom_shader_eval_indices.clear();

        auto& materials = global_payload.get_materials();
        int next_eval_index = 3;
        for (auto& material : materials) {
            if (material.second == nullptr) {
                continue;
            }

            auto location = material.second->GetMaterialLocation();
            if (location == static_cast<unsigned>(-1)) {
                continue;
            }

            if (material.second->HasValidShader()) {
                std::filesystem::path shader_path(
                    material.second->GetShaderPath());
                if (!shader_path.is_absolute()) {
                    shader_path = std::filesystem::path(RENDERER_SHADER_DIR) /
                                  material.second->GetShaderPath();
                }
                program_desc.add_path(shader_path.string());

                storage.custom_shader_eval_indices[location] = next_eval_index;
                storage.callable_shaders[location] =
                    material.second->GetMaterialName();

                std::string fetch_wrapper =
                    R"(
import callable_data;
import Scene.BindlessMaterial;

[shader("callable")]
void fetch_)" + material.second->GetMaterialName() +
                    R"((inout FetchCallableData data)
{
    data.shader_type_id = )" +
                    std::to_string(next_eval_index) + R"(;
}

[shader("callable")]
void fetch_)" + material.second->GetMaterialName() +
                    R"(_opacity(inout FetchCallableData data)
{
    data.shader_type_id = )" +
                    std::to_string(next_eval_index) + R"(;
    data.material_params_index = asuint(1.0f);
}
)";
                program_desc.add_source_code(fetch_wrapper);
                next_eval_index++;
            }
            else {
                program_desc.add_source_code(
                    material.second->GetShader(shader_factory));
                storage.callable_shaders[location] =
                    material.second->GetMaterialName();
            }
        }

        if (storage.program) {
            resource_allocator.destroy(storage.program);
        }
        storage.program = resource_allocator.create(program_desc);
        CHECK_PROGRAM_ERROR(storage.program);
    }

    if (size_changed || !storage.output) {
        storage.output =
            create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
        initialize_texture(params, storage.output, nvrhi::Color(0.f, 0.f, 0.f, 1.f));
    }

    bool rebuild_bindings =
        mat_dirty || light_dirty || size_changed || settings_changed ||
        !storage.cached_program_vars || !storage.cached_rt_context;

    if (rebuild_bindings) {
        g.reset_accumulation = true;
        storage.cached_max_depth = max_depth;
        storage.cached_rr_start_depth = rr_start_depth;
        storage.cached_rr_floor = rr_floor;

        spdlog::info(
            "HW7 path tracing rebuilding bindings (geom_dirty={}, mat_dirty={}, light_dirty={}, size_changed={}, settings_changed={})",
            geom_dirty,
            mat_dirty,
            light_dirty,
            size_changed,
            settings_changed);
        spdlog::info(
            "HW7 path tracing settings: max_depth={} rr_start_depth={} rr_floor={}",
            max_depth,
            rr_start_depth,
            rr_floor);

        spdlog::info("HW7 path tracing: creating ProgramVars");
        storage.cached_program_vars = std::make_unique<ProgramVars>(
            resource_allocator, storage.program);
        spdlog::info("HW7 path tracing: ProgramVars created");
        ProgramVars& program_vars = *storage.cached_program_vars;

        SamplerDesc sampler_desc;
        sampler_desc.addressU = nvrhi::SamplerAddressMode::Wrap;
        sampler_desc.addressV = nvrhi::SamplerAddressMode::Wrap;

        if (storage.sampler) {
            resource_allocator.destroy(storage.sampler);
        }
        storage.sampler = resource_allocator.create(sampler_desc);
        for (int i = 0; i < 9; ++i) {
            program_vars["samplers"][i] = storage.sampler;
        }
        spdlog::info("HW7 path tracing: samplers bound");

        auto random_seeds =
            params.get_input<nvrhi::BufferHandle>("Random Seeds");
        auto rays = params.get_input<nvrhi::BufferHandle>("Rays");
        auto pixel_target = params.get_input<nvrhi::BufferHandle>("Pixel Target");

        instance_collection->light_pool.compress();
        auto light_buffer = instance_collection->light_pool.get_device_buffer();
        uint32_t light_count =
            static_cast<uint32_t>(instance_collection->light_pool.count());
        if (!light_buffer) {
            light_count = 0;
            light_buffer = create_buffer<LightData>(params, 1, LightData{});
        }
        spdlog::info(
            "HW7 path tracing: light buffer ready (light_count={}, has_buffer={})",
            light_count,
            light_buffer != nullptr);

        nvrhi::BufferDesc material_params_desc;
        material_params_desc.byteSize =
            rays->getDesc().byteSize / sizeof(RayInfo) * sizeof(MaterialParams);
        material_params_desc.structStride = sizeof(MaterialParams);
        material_params_desc.canHaveUAVs = true;
        material_params_desc.initialState =
            nvrhi::ResourceStates::ShaderResource;
        material_params_desc.debugName = "hw7MaterialParamsBuffer";
        material_params_desc.keepInitialState = true;

        if (storage.material_params_buffer) {
            resource_allocator.destroy(storage.material_params_buffer);
        }
        storage.material_params_buffer =
            resource_allocator.create(material_params_desc);
        spdlog::info("HW7 path tracing: material params buffer created");

        HW7PathTracingConstants constants = {
            light_count,
            static_cast<uint32_t>(
                3 + storage.custom_shader_eval_indices.size()),
            static_cast<uint32_t>(storage.callable_shaders.size()),
            static_cast<uint32_t>(max_depth),
            static_cast<uint32_t>(rr_start_depth),
            rr_floor
        };

        if (storage.constants_buffer) {
            resource_allocator.destroy(storage.constants_buffer);
        }
        storage.constants_buffer = create_constant_buffer(params, constants);
        spdlog::info("HW7 path tracing: constants buffer created");

        program_vars["SceneBVH"] = instance_collection->get_tlas();
        program_vars["inPixelTarget"] = pixel_target;
        program_vars["rays"] = rays;
        program_vars["random_seeds"] = random_seeds;
        program_vars["output"] = storage.output;
        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();
        program_vars["materialBlobBuffer"] =
            instance_collection->material_pool.get_device_buffer();
        program_vars["materialHeaderBuffer"] =
            instance_collection->material_header_pool.get_device_buffer();
        program_vars["materialParamsBuffer"] = storage.material_params_buffer;
        program_vars["lightBuffer"] = light_buffer;
        program_vars["hw7Constants"] = storage.constants_buffer;
        spdlog::info("HW7 path tracing: core buffers bound");

        program_vars.set_descriptor_table(
            "t_BindlessBuffers",
            instance_collection->bindlessData.bufferDescriptorTableManager
                ->GetDescriptorTable(),
            instance_collection->bindlessData.bufferBindlessLayout);
        spdlog::info("HW7 path tracing: bindless buffer table bound");

        program_vars.set_descriptor_table(
            "t_BindlessTextures",
            instance_collection->bindlessData.textureDescriptorTableManager
                ->GetDescriptorTable(),
            instance_collection->bindlessData.textureBindlessLayout);
        spdlog::info("HW7 path tracing: bindless texture table bound");

        spdlog::info("HW7 path tracing: finalizing ProgramVars");
        program_vars.finish_setting_vars();
        spdlog::info("HW7 path tracing: ProgramVars finalized");

        spdlog::info("HW7 path tracing: creating RaytracingContext");
        storage.cached_rt_context = std::make_unique<RaytracingContext>(
            resource_allocator, program_vars);
        spdlog::info("HW7 path tracing: RaytracingContext created");

        auto& context = *storage.cached_rt_context;
        context.set_max_recursion_depth(2);
        context.set_max_payload_size(256);
        context.set_max_attribute_size(4 * sizeof(float));
        spdlog::info(
            "HW7 path tracing: set ray tracing limits recursion_depth=2 payload_bytes=256 attribute_bytes=16");

        context.announce_raygeneration("RayGen");
        context.announce_hitgroup("ClosestHit", "", "", 0);
        context.announce_hitgroup("ShadowHit", "", "", 1);
        context.announce_miss("Miss", 0);
        context.announce_miss("ShadowMiss", 1);
        spdlog::info("HW7 path tracing: base shader entries announced");

        context.announce_callable("eval_standard_surface", 0, nullptr);
        context.announce_callable("eval_preview_surface", 1, nullptr);
        context.announce_callable("eval_fallback", 2, nullptr);
        spdlog::info("HW7 path tracing: shared eval callables announced");

        for (auto& entry : storage.custom_shader_eval_indices) {
            unsigned material_location = entry.first;
            unsigned eval_index = entry.second;
            std::string callable_name =
                "eval_" + storage.callable_shaders[material_location];
            context.announce_callable(callable_name, eval_index, nullptr);
        }
        spdlog::info("HW7 path tracing: custom eval callables announced");

        int base_fetch_index =
            3 + static_cast<int>(storage.custom_shader_eval_indices.size());
        for (auto& callable : storage.callable_shaders) {
            std::string fetch_name =
                storage.custom_shader_eval_indices.count(callable.first) > 0
                    ? "fetch_" + callable.second
                    : callable.second;
            context.announce_callable(
                fetch_name, base_fetch_index + callable.first, nullptr);
        }
        spdlog::info("HW7 path tracing: fetch callables announced");

        int base_opacity_index =
            base_fetch_index + static_cast<int>(storage.callable_shaders.size());
        for (auto& callable : storage.callable_shaders) {
            std::string opacity_name =
                storage.custom_shader_eval_indices.count(callable.first) > 0
                    ? "fetch_" + callable.second + "_opacity"
                    : callable.second + "_opacity";
            context.announce_callable(
                opacity_name, base_opacity_index + callable.first, nullptr);
        }
        spdlog::info("HW7 path tracing: opacity fetch callables announced");

        spdlog::info("HW7 path tracing: finishing shader name announcements");
        {
            std::lock_guard<std::mutex> gpu_lock(execution_launch_mutex);
            spdlog::info(
                "HW7 path tracing: acquired GPU lock for ray tracing pipeline finalization");
            context.finish_announcing_shader_names();
            spdlog::info(
                "HW7 path tracing: released GPU lock after ray tracing pipeline finalization");
        }
        spdlog::info("HW7 path tracing: shader name announcements finished");
    }
    else if (geom_dirty) {
        g.reset_accumulation = true;

        ProgramVars& program_vars = *storage.cached_program_vars;
        program_vars["SceneBVH"] = instance_collection->get_tlas();
        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();
        program_vars.finish_setting_vars();

        spdlog::info(
            "HW7 path tracing: updated geometry buffers after geometry dirty");
    }

    auto buffer_size =
        params.get_input<nvrhi::BufferHandle>("Rays")->getDesc().byteSize /
        sizeof(RayInfo);
    if (buffer_size > 0) {
        spdlog::info("HW7 path tracing dispatching {} rays", buffer_size);
        std::lock_guard<std::mutex> gpu_lock(execution_launch_mutex);
        spdlog::info("HW7 path tracing: acquired GPU lock for dispatch");
        storage.cached_rt_context->begin();
        storage.cached_rt_context->trace_rays(
            {}, *storage.cached_program_vars, buffer_size, 1, 1);
        storage.cached_rt_context->finish();
        spdlog::info("HW7 path tracing: released GPU lock after dispatch");
    }

    params.set_output("Output", storage.output);
    return true;
}

NODE_DECLARATION_UI(hw7_path_tracing);
NODE_DEF_CLOSE_SCOPE
