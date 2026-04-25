#pragma once

#include "GPUContext/program_vars.hpp"
#include "api.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/materialNetwork2Interface.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#include "pxr/imaging/hio/image.h"
#include "renderParam.h"

namespace pxr {
class Hio_OpenEXRImage;

}

RUZINO_NAMESPACE_OPEN_SCOPE
class Shader;
using namespace pxr;

class Hio_StbImage;

struct HW7PreviewMaterialData {
    pxr::GfVec4f baseColorRoughness = pxr::GfVec4f(0.8f, 0.8f, 0.8f, 0.5f);
    pxr::GfVec4f emissiveMetallic = pxr::GfVec4f(0.f, 0.f, 0.f, 0.f);
};

class HD_RUZINO_API Hd_RUZINO_Material : public HdMaterial {
   public:
    explicit Hd_RUZINO_Material(SdfPath const& id);

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Finalize(HdRenderParam* renderParam) override;
    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;
    void ensure_material_data_handle(Hd_RUZINO_RenderParam* render_param);

    virtual void ensure_shader_ready(const ShaderFactory& factory);

    // Get shader generation counter to track changes
    uint32_t get_shader_generation() const
    {
        return shader_generation;
    }

    unsigned GetMaterialLocation() const;

    std::string GetShader(const ShaderFactory& factory);

    std::string GetMaterialName() const;

    const HW7PreviewMaterialData& GetHW7PreviewMaterialData() const
    {
        return hw7_preview_material_data_;
    }

    // Upload material data to GPU (override in subclasses if needed)
    virtual void upload_material_data()
    ;

    virtual void update_data_loader(
        DescriptorIndex descriptor_index,
        const std::string& texture_name)
    {
    }

    // Accessor for shader path (for custom callable shaders)
    [[nodiscard]] const std::string& GetShaderPath() const
    {
        return shader_path;
    }
    [[nodiscard]] bool HasValidShader() const
    {
        return has_valid_shader;
    }

   protected:
    HdMaterialNetwork2 surfaceNetwork;

    std::string eval_shader_source;
    std::string material_name;
    std::string final_shader_source;

    bool shader_ready = false;

    std::unordered_map<std::string, std::string> texturePaths;
    ProgramHandle final_program;

    struct TextureResource {
        std::string filePath;
        HioImageSharedPtr image;
        nvrhi::TextureHandle texture;
        DescriptorHandle descriptor;
        bool isSRGB =
            false;  // Track whether this texture should use sRGB format
    };

    std::unordered_map<std::string, TextureResource> textureResources;

    HdMaterialNetwork2Interface FetchNetInterface(
        HdSceneDelegate* sceneDelegate,
        HdMaterialNetwork2& hdNetwork,
        SdfPath& materialPath);

    DeviceMemoryPool<MaterialDataBlob>::MemoryHandle material_data_handle;
    DeviceMemoryPool<MaterialHeader>::MemoryHandle material_header_handle;
    MaterialDataBlob material_data;
    MaterialHeader material_header = {};
    bool material_header_dirty = false;

    std::string slang_source_code_main;
    static std::string slang_source_code_template;
    static std::string eval_source_code_fallback;
    static std::mutex texture_mutex;
    static std::mutex material_data_handle_mutex;
    HW7PreviewMaterialData hw7_preview_material_data_;

    uint32_t shader_generation = 0;  // Incremented when shader is regenerated

    // Path to custom callable shader file
    std::string shader_path;
    bool has_valid_shader =
        false;  // True only if shader_path points to a valid file

    void set_hw7_preview_material_data(const HW7PreviewMaterialData& data)
    {
        hw7_preview_material_data_ = data;
    }
};

RUZINO_NAMESPACE_CLOSE_SCOPE
