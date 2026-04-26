#include "renderer.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>

#include "camera.h"
#include "config.h"
#include "geometries/mesh.h"
#include "light.h"
#include "material/material.h"
#include "node_exec_eager_render.hpp"
#include "nodes/system/node_system.hpp"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/tokens.h"
#include "renderBuffer.h"
#include "renderParam.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Renderer::Hd_RUZINO_Renderer(Hd_RUZINO_RenderParam* render_param)
    : _enableSceneColors(false),
      render_param(render_param)
{
}

Hd_RUZINO_Renderer::~Hd_RUZINO_Renderer()
{
    auto executor = dynamic_cast<EagerNodeTreeExecutorRender*>(
        render_param->node_system->get_node_tree_executor());
}

static TextureHandle create_empty_texture(
    const pxr::GfVec2i& size,
    nvrhi::Format format = nvrhi::Format::RGBA32_FLOAT)
{
    nvrhi::TextureDesc desc =
        nvrhi::TextureDesc{}
            .setWidth(size[0])
            .setHeight(size[1])
            .setFormat(format)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setIsUAV(true);
    auto d = RHI::get_device();
    auto texture = d->createTexture(desc);

    auto commandList = d->createCommandList();
    commandList->open();
    commandList->clearTextureFloat(
        texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
    commandList->close();
    d->executeCommandList(commandList);
    d->waitForIdle();

    return texture;
}

void Hd_RUZINO_Renderer::Render(HdRenderThread* renderThread)
{
    spdlog::info("Hd_RUZINO_Renderer::Render begin");
    _completedSamples.store(0);

    render_param->default_texture_name.clear();
    {
        std::scoped_lock lock(render_param->presented_textures_mutex);
        render_param->presented_textures.clear();
        render_param->current_present_texture = nullptr;
    }

    for (auto& material_thread : render_param->material_loading_threads) {
        if (material_thread.joinable()) {
            material_thread.join();
        }
    }
    render_param->material_loading_threads.clear();

    for (auto& texture_thread : render_param->texture_loading_threads) {
        if (texture_thread.joinable()) {
            texture_thread.join();
        }
    }
    render_param->texture_loading_threads.clear();

    // Upload material data to GPU after all textures are loaded
    for (auto& material : *render_param->material_map) {
        if (!material.second) {
            continue;
        }

        spdlog::info(
            "Hd_RUZINO_Renderer::Render uploading material {}",
            material.first.GetText());
        material.second->upload_material_data();
        spdlog::info(
            "Hd_RUZINO_Renderer::Render material {} uploaded",
            material.first.GetText());
    }

    const int target_samples = static_cast<int>(
        Hd_RUZINO_Config::GetInstance().samplesToConvergence);
    auto node_system = render_param->node_system;
    auto& global_payload = node_system->get_node_tree_executor()
                               ->get_global_payload<RenderGlobalPayload&>();

    auto find_required_present_node = [&]() -> Node* {
        bool need_color = false;
        bool need_depth = false;
        for (const auto& binding : _aovBindings) {
            if (binding.aovName == HdAovTokens->color) {
                need_color = true;
            }
            if (binding.aovName == HdAovTokens->depth) {
                need_depth = true;
            }
        }

        Node* depth_candidate = nullptr;
        for (auto&& node : node_system->get_node_tree()->nodes) {
            if (std::string(node->typeinfo->id_name) == "present_color" &&
                need_color) {
                return node.get();
            }
            if (std::string(node->typeinfo->id_name) == "present_depth" &&
                need_depth && !depth_candidate) {
                depth_candidate = node.get();
            }
        }
        return depth_candidate;
    };

    auto publish_presented_textures = [&](bool frame_converged) {
        for (size_t i = 0; i < _aovBindings.size(); ++i) {
            std::string present_name;

            if (_aovBindings[i].aovName == HdAovTokens->depth) {
                present_name = "present_depth";
            }

            if (_aovBindings[i].aovName == HdAovTokens->color) {
                present_name = "present_color";
            }

            for (auto&& node : node_system->get_node_tree()->nodes) {
                if (std::string(node->typeinfo->id_name) != present_name) {
                    continue;
                }

                assert(node->get_inputs().size() == 1);
                auto output_socket = node->get_inputs()[0];
                entt::meta_any data;
                auto* executor = node_system->get_node_tree_executor();
                executor->sync_node_to_external_storage(output_socket, data);

                if (!data) {
                    if (auto* direct_value = executor->get_socket_value(output_socket);
                        direct_value && *direct_value) {
                        data = *direct_value;
                    }
                }

                if (!data) {
                    continue;
                }

                nvrhi::TextureHandle texture = data.cast<nvrhi::TextureHandle>();
                if (!texture) {
                    continue;
                }

                std::string texture_name =
                    node->ui_name.empty() ? present_name : node->ui_name;
                {
                    std::scoped_lock lock(render_param->presented_textures_mutex);
                    render_param->presented_textures[texture_name] = texture;
                    if (render_param->default_texture_name.empty()) {
                        render_param->default_texture_name = texture_name;
                    }
                    if (render_param->default_texture_name == texture_name) {
                        render_param->current_present_texture = texture;
                    }
                }

                auto rb = static_cast<Hd_RUZINO_RenderBuffer*>(
                    _aovBindings[i].renderBuffer);
#ifdef RUZINO_DIRECT_VK_DISPLAY
                // Already stored above
#else
                rb->Present(texture);
#endif
                rb->SetConverged(frame_converged);
            }

            if (render_param->default_texture_name.empty()) {
                auto empty_tex = create_empty_texture(
                    GfVec2i{ 16, 16 }, nvrhi::Format::RGBA32_FLOAT);
                {
                    std::scoped_lock lock(render_param->presented_textures_mutex);
                    render_param->presented_textures["_empty"] = empty_tex;
                    render_param->current_present_texture = empty_tex;
                }
                render_param->default_texture_name = "_empty";
            }
        }
    };

    bool reset_accumulation_once = pending_accumulation_reset_.exchange(false);

    while (true) {
        if (renderThread && renderThread->IsStopRequested()) {
            break;
        }

        while (renderThread && renderThread->IsPauseRequested()) {
            if (renderThread->IsStopRequested()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (renderThread && renderThread->IsStopRequested()) {
            break;
        }

        for (auto* light : global_payload.get_lights()) {
            if (!light) {
                continue;
            }
            light->upload_light_data();
        }

        for (auto* mesh : global_payload.get_meshes()) {
            if (!mesh) {
                continue;
            }
            mesh->upload_gpu_data(render_param);
        }

        global_payload.clear_dirty(
            RenderGlobalPayload::SceneDirtyBits::DirtyMaterials);
        global_payload.clear_dirty(
            RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);
        global_payload.clear_dirty(
            RenderGlobalPayload::SceneDirtyBits::DirtyLights);

        global_payload.InstanceCollection = render_param->InstanceCollection.get();

        {
            std::vector<std::future<void>> futures;

            for (auto& material : *render_param->material_map) {
                if (!material.second) {
                    continue;
                }

                auto material_path = material.first;
                futures.push_back(
                    std::async(std::launch::async, [&, material_path]() {
                        auto mat = (*render_param->material_map)[material_path];
                        if (!mat) {
                            return;
                        }

                        mat->ensure_shader_ready(global_payload.shader_factory);
                    }));
            }

            for (auto& future : futures) {
                future.wait();
            }
        }

        static uint32_t last_material_version = 0;
        uint32_t current_material_version =
            global_payload.InstanceCollection->get_material_version();
        if (last_material_version != current_material_version) {
            global_payload.mark_dirty(
                RenderGlobalPayload::SceneDirtyBits::DirtyMaterials);
            last_material_version = current_material_version;
        }

        static uint32_t last_geometry_version = 0;
        uint32_t current_geometry_version =
            global_payload.InstanceCollection->get_geometry_version();
        if (last_geometry_version != current_geometry_version) {
            global_payload.mark_dirty(
                RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);
            last_geometry_version = current_geometry_version;
        }

        static uint32_t last_light_version = 0;
        uint32_t current_light_version =
            global_payload.InstanceCollection->get_light_version();
        if (last_light_version != current_light_version) {
            global_payload.mark_dirty(
                RenderGlobalPayload::SceneDirtyBits::DirtyLights);
            last_light_version = current_light_version;
        }

        global_payload.resource_allocator.gc();
        global_payload.lens_system = render_param->lens_system;
        global_payload.reset_accumulation = reset_accumulation_once;
        reset_accumulation_once = false;

        Node* required_present_node = find_required_present_node();
        if (!required_present_node) {
            spdlog::warn(
                "Hd_RUZINO_Renderer::Render found no explicit present node to execute toward; falling back to default execution");
        }
        node_system->execute(false, required_present_node);

        const int completed_samples = _completedSamples.fetch_add(1) + 1;
        const bool frame_converged = completed_samples >= target_samples;
        publish_presented_textures(frame_converged);

        if (frame_converged) {
            break;
        }
    }

    node_system->finalize();

    // executor->finalize(node_tree);
    spdlog::info("Hd_RUZINO_Renderer::Render end");
}

void Hd_RUZINO_Renderer::Clear()
{
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        if (_aovBindings[i].clearValue.IsEmpty()) {
            continue;
        }

        auto rb =
            static_cast<Hd_RUZINO_RenderBuffer*>(_aovBindings[i].renderBuffer);
        rb->Clear();
    }
}

void Hd_RUZINO_Renderer::SetAovBindings(
    const HdRenderPassAovBindingVector& aovBindings)
{
    _aovBindings = aovBindings;
    _aovNames.resize(_aovBindings.size());
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        _aovNames[i] = HdParsedAovToken(_aovBindings[i].aovName);
    }

    // Re-validate the attachments.
    _aovBindingsNeedValidation = true;
}

int Hd_RUZINO_Renderer::GetCompletedSamples() const
{
    return _completedSamples.load();
}

void Hd_RUZINO_Renderer::MarkAovBuffersUnconverged()
{
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        auto rb =
            static_cast<Hd_RUZINO_RenderBuffer*>(_aovBindings[i].renderBuffer);
        rb->SetConverged(false);
    }
}

void Hd_RUZINO_Renderer::renderTimeUpdateCamera(
    const HdRenderPassStateSharedPtr& renderPassState)
{
    camera_ =
        static_cast<const Hd_RUZINO_Camera*>(renderPassState->GetCamera());
    if (camera_)
        camera_->update(renderPassState);
}

bool Hd_RUZINO_Renderer::nodetree_modified()
{
    if (!render_param || !render_param->node_system) {
        return false;
    }

    auto* tree = render_param->node_system->get_node_tree();
    return tree ? tree->GetDirty() : false;
}

bool Hd_RUZINO_Renderer::nodetree_modified(bool new_status)
{
    if (!render_param || !render_param->node_system) {
        return false;
    }

    auto* tree = render_param->node_system->get_node_tree();
    if (!tree) {
        return false;
    }

    const bool old_status = tree->GetDirty();
    tree->SetDirty(new_status);
    if (old_status && !new_status) {
        pending_accumulation_reset_.store(true);
    }
    return old_status;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
