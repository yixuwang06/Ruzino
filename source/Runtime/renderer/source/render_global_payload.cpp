#include <hd_RUZINO/render_global_payload.hpp>

#include "RHI/rhi.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE
RenderGlobalPayload::RenderGlobalPayload()
{
}

RenderGlobalPayload::RenderGlobalPayload(
    std::vector<Hd_RUZINO_Camera*>* cameras,
    std::vector<Hd_RUZINO_Light*>* lights,
    std::vector<Hd_RUZINO_Mesh*>* meshes,
    pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>* materials,
    nvrhi::IDevice* nvrhi_device)
    : cameras(cameras),
      lights(lights),
      meshes(meshes),
      materials(materials),
      nvrhi_device(nvrhi_device),
      shader_factory(&resource_allocator)
{
    shader_factory.set_search_path(RENDERER_SHADER_DIR);
    shader_factory.add_search_path("usd/hd_RUZINO/resources/libraries");
    resource_allocator.device = RHI::get_device();
    resource_allocator.shader_factory = &shader_factory;
}

RenderGlobalPayload::RenderGlobalPayload(const RenderGlobalPayload& rhs)
    : cameras(rhs.cameras),
      lights(rhs.lights),
      meshes(rhs.meshes),
      materials(rhs.materials),
      nvrhi_device(rhs.nvrhi_device),
      shader_factory(&resource_allocator)
{
    shader_factory.set_search_path(RENDERER_SHADER_DIR);
    shader_factory.add_search_path("usd/hd_RUZINO/resources/libraries");

    resource_allocator.device = nvrhi_device;
    resource_allocator.shader_factory = &shader_factory;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
