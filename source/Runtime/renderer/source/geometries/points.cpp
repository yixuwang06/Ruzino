//
// Point/Sphere Geometry Implementation
//
#include "points.h"

#include <spdlog/spdlog.h>

#include "../gpu_compute.h"
#include "../instancer.h"
#include "../renderParam.h"
#include "Scene/SceneTypes.slang"
#include "material/material.h"
#include "nvrhi/utils.h"
#include "pxr/imaging/hd/instancer.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Points::Hd_RUZINO_Points(const SdfPath& id)
    : HdPoints(id),
      _pointsValid(false)
{
    auto device = RHI::get_device();
    copy_commandlist = device->createCommandList();
}

Hd_RUZINO_Points::~Hd_RUZINO_Points()
{
}

HdDirtyBits Hd_RUZINO_Points::GetInitialDirtyBitsMask() const
{
    int mask = HdChangeTracker::Clean | HdChangeTracker::InitRepr |
               HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTransform |
               HdChangeTracker::DirtyVisibility |
               HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyWidths |
               HdChangeTracker::DirtyInstancer |
               HdChangeTracker::DirtyMaterialId;

    return (HdDirtyBits)mask;
}

HdDirtyBits Hd_RUZINO_Points::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void Hd_RUZINO_Points::create_gpu_resources(Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();

    if (!copy_commandlist)
        copy_commandlist = device->createCommandList(
            nvrhi::CommandListParameters{}.setQueueType(
                nvrhi::CommandQueue::Copy));

    // Calculate buffer layout
    size_t position_buffer_offset = 0;
    size_t radius_buffer_offset = 0;

    // Position buffer: 3 floats per point (x, y, z)
    size_t total_buffer_size = points.size() * 3 * sizeof(float);
    radius_buffer_offset = total_buffer_size;

    // Radius buffer: 1 float per point
    total_buffer_size += widths.size() * sizeof(float);

    if (!vertexBuffer || vertexBuffer->getDesc().byteSize != total_buffer_size)

    {
        // Create vertex buffer for positions and radii
        nvrhi::BufferDesc desc =
            nvrhi::BufferDesc{}
                .setCanHaveRawViews(true)
                .setByteSize(total_buffer_size)
                .setIsVertexBuffer(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setCpuAccess(nvrhi::CpuAccessMode::None)
                .setIsAccelStructBuildInput(true)
                .setKeepInitialState(true)
                .setDebugName("sphereVertexBuffer");
        vertexBuffer = device->createBuffer(desc);
    }

    // Upload data to GPU
    copy_commandlist->open();

    // Write positions
    copy_commandlist->writeBuffer(
        vertexBuffer,
        points.data(),
        points.size() * 3 * sizeof(float),
        position_buffer_offset);

    // Write radii (widths are already radii, no conversion needed)
    copy_commandlist->writeBuffer(
        vertexBuffer,
        widths.data(),
        widths.size() * sizeof(float),
        radius_buffer_offset);

    copy_commandlist->close();

    {
        std::lock_guard lock(execution_launch_mutex);
        device->executeCommandList(copy_commandlist);

        // Create AABB buffer if not already created or size changed
        size_t required_aabb_size =
            points.size() * sizeof(nvrhi::rt::GeometryAABB);
        if (!aabbBuffer ||
            aabbBuffer->getDesc().byteSize != required_aabb_size) {
            nvrhi::BufferDesc aabb_desc =
                nvrhi::BufferDesc{}
                    .setByteSize(required_aabb_size)
                    .setStructStride(sizeof(nvrhi::rt::GeometryAABB))
                    .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                    .setKeepInitialState(true)
                    .setCanHaveUAVs(true)
                    .setDebugName("sphere_aabbs");
            aabbBuffer = device->createBuffer(aabb_desc);
        }

        // Use GPU to compute AABBs from sphere positions and radii
        GPUSceneAssember::compute_sphere_aabbs(
            vertexBuffer,
            position_buffer_offset,
            radius_buffer_offset,
            points.size(),
            aabbBuffer);

        // Build BLAS for spheres using AABBs
        nvrhi::rt::AccelStructDesc blas_desc;
        nvrhi::rt::GeometryDesc geometry_desc;
        geometry_desc.geometryType = nvrhi::rt::GeometryType::AABBs;
        geometry_desc.useTransform = false;

        nvrhi::rt::GeometryAABBs aabbGeometry;
        aabbGeometry.setBuffer(aabbBuffer)
            .setCount(points.size())
            .setStride(sizeof(nvrhi::rt::GeometryAABB))
            .setOffset(0);

        geometry_desc.setAABBs(aabbGeometry);
        blas_desc.addBottomLevelGeometry(geometry_desc);
        blas_desc.isTopLevel = false;

        BLAS = device->createAccelStruct(blas_desc);
        if (!command_list)
            command_list = device->createCommandList();
        command_list->open();
        nvrhi::utils::BuildBottomLevelAccelStruct(
            command_list, BLAS, blas_desc);
        command_list->close();
        device->executeCommandList(command_list);
        device->waitForIdle();

        auto descriptor_table =
            render_param->InstanceCollection->get_buffer_descriptor_table();
        descriptor_handle = descriptor_table->CreateDescriptorHandle(
            nvrhi::BindingSetItem::RawBuffer_SRV(0, vertexBuffer));
    }

    // Create mesh descriptor for sphere vertex buffer
    const SdfPath& id = GetId();
    MeshDesc mesh_desc;
    mesh_desc.vbOffset = position_buffer_offset;
    mesh_desc.bindlessIndex = descriptor_handle.Get();
    mesh_desc.ibOffset =
        radius_buffer_offset;  // Store radius buffer offset in ibOffset
    mesh_desc.normalOffset = 0;
    mesh_desc.tangentOffset = 0;
    mesh_desc.texCrdOffset = 0;
    mesh_desc.subsetMatIdOffset = 0;
    mesh_desc.flags = 0;

    mesh_desc_buffer = render_param->InstanceCollection->mesh_pool.allocate(1);
    mesh_desc_buffer->write_data(&mesh_desc);

    spdlog::info(
        "Points {}: created mesh descriptor at index {}",
        id.GetText(),
        mesh_desc_buffer->index());

    spdlog::info("Created sphere BLAS with {} points", points.size());
}

void Hd_RUZINO_Points::updateTLAS(
    Hd_RUZINO_RenderParam* render_param,
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits* dirtyBits)
{
    _UpdateInstancer(sceneDelegate, dirtyBits);
    const SdfPath& id = GetId();

    HdInstancer::_SyncInstancerAndParents(
        sceneDelegate->GetRenderIndex(), GetInstancerId());

    auto material_id = GetMaterialId();

    if (material_id.IsEmpty()) {
        spdlog::warn("Points {} has no material assigned", id.GetText());
    }

    Hd_RUZINO_Material* material = (*render_param->material_map)[material_id];
    if (!material) {
        spdlog::warn(
            "Material {} not found for points {}. Continuing without material.",
            material_id.GetText(),
            id.GetText());
    }

    size_t instance_count = 1;

    // Determine instance count
    if (!GetInstancerId().IsEmpty()) {
        HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
        HdInstancer* instancer = renderIndex.GetInstancer(GetInstancerId());
        VtIntArray instanceIndices =
            sceneDelegate->GetInstanceIndices(GetInstancerId(), GetId());
        instance_count = instanceIndices.size();
        spdlog::info(
            "Points {} has instancer {} with {} instances",
            id.GetText(),
            GetInstancerId().GetText(),
            instance_count);
    }
    else {
        spdlog::info(
            "Points {} has no instancer, using single instance", id.GetText());
    }

    auto& rt_instance_pool = render_param->InstanceCollection->rt_instance_pool;

    if (!rt_instanceBuffer || rt_instanceBuffer->count() != instance_count)
        rt_instanceBuffer = rt_instance_pool.allocate(instance_count);
    if (!instanceBuffer || instanceBuffer->count() != instance_count)
        instanceBuffer =
            render_param->InstanceCollection->instance_pool.allocate(
                instance_count);

    if (material) {
        material->ensure_material_data_handle(render_param);
    }

    // CPU path: Single instance or manual instancing
    GeometryInstanceData instance_data;
    instance_data.geometryID =
        mesh_desc_buffer->index();  // Use sphere mesh descriptor
    instance_data.materialID = material ? material->GetMaterialLocation() : -1;
    memcpy(&instance_data.transform, transform.data(), sizeof(pxr::GfMatrix4f));
    instance_data.flags = 0;

    instanceBuffer->write_data(&instance_data);

    nvrhi::rt::InstanceDesc rt_instance;
    rt_instance.blasDeviceAddress = BLAS->getDeviceAddress();
    rt_instance.instanceMask = 1;
    rt_instance.instanceContributionToHitGroupIndex =
        2;  // Use sphere hit groups
    rt_instance.flags = nvrhi::rt::InstanceFlags::None;

    GfMatrix4f mat_transposed = transform.GetTranspose();
    memcpy(
        rt_instance.transform,
        mat_transposed.data(),
        sizeof(nvrhi::rt::AffineTransform));
    rt_instance.instanceID = instanceBuffer->index();

    rt_instanceBuffer->write_data(&rt_instance);

    render_param->InstanceCollection->set_require_rebuild_tlas();

    spdlog::info(
        "Updated TLAS for points {} with {} instances",
        id.GetText(),
        instance_count);
}

void Hd_RUZINO_Points::_InitRepr(
    const TfToken& reprToken,
    HdDirtyBits* dirtyBits)
{
}

void Hd_RUZINO_Points::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits,
    const TfToken& reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();
    bool had_scene_dirty = (*dirtyBits != HdChangeTracker::Clean);

    const SdfPath& id = GetId();
    Hd_RUZINO_RenderParam* render_param =
        static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    bool update_gpu_resources = false;

    // Handle transform
    if (*dirtyBits & HdChangeTracker::DirtyTransform) {
        transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }

    // Handle points data
    if (*dirtyBits & HdChangeTracker::DirtyPoints) {
        VtValue pointsValue = sceneDelegate->Get(id, HdTokens->points);
        points = pointsValue.Get<VtArray<GfVec3f>>();
        _pointsValid = true;
        update_gpu_resources = true;

        spdlog::info(
            "Points {}: loaded {} points", id.GetText(), points.size());
    }

    // Handle widths/radii
    if (*dirtyBits & HdChangeTracker::DirtyWidths) {
        VtValue widthsValue = sceneDelegate->Get(id, HdTokens->widths);
        if (!widthsValue.IsEmpty()) {
            widths = widthsValue.Get<VtFloatArray>();
        }
        else {
            // Default width if not specified
            widths.resize(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                widths[i] = 0.1f;  // Default radius
            }
        }
        update_gpu_resources = true;

        spdlog::info(
            "Points {}: loaded {} widths", id.GetText(), widths.size());
    }

    // Handle material
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        SdfPath const& newMaterialId = sceneDelegate->GetMaterialId(id);
        if (GetMaterialId() != newMaterialId) {
            SetMaterialId(newMaterialId);
            spdlog::info(
                "Points {}: material updated to {}",
                id.GetText(),
                newMaterialId.GetText());
        }
    }

    // Create or update GPU resources
    if (update_gpu_resources && _pointsValid && !points.empty()) {
        create_gpu_resources(render_param);
    }

    // Update TLAS
    if (_pointsValid && BLAS) {
        updateTLAS(render_param, sceneDelegate, dirtyBits);
    }
    if (had_scene_dirty) {
        static_cast<Hd_RUZINO_RenderParam*>(renderParam)
            ->InstanceCollection->mark_geometry_dirty();
    }

    *dirtyBits = HdChangeTracker::Clean;
}

void Hd_RUZINO_Points::Finalize(HdRenderParam* renderParam)
{
    Hd_RUZINO_RenderParam* render_param =
        static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    if (instanceBuffer)
        instanceBuffer.reset();
    if (rt_instanceBuffer)
        rt_instanceBuffer.reset();
    if (mesh_desc_buffer)
        mesh_desc_buffer.reset();

    spdlog::info("Finalized points {}", GetId().GetText());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
