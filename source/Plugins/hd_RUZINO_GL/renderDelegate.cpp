//
// Copyright 2020 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
// #

#include "renderDelegate.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <regex>

#include "camera.h"
#include "config.h"
#include "geometries/mesh.h"
#include "global_payload_gl.hpp"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "node_exec_eager_render.hpp"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/extComputation.h"
#include "renderBuffer.h"
#include "renderPass.h"
#include "renderer.h"
RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;
TF_DEFINE_PUBLIC_TOKENS(
    HdEmbreeRenderSettingsTokens,
    HDEMBREE_RENDER_SETTINGS_TOKENS);

const TfTokenVector Hd_RUZINO_RenderDelegate::SUPPORTED_RPRIM_TYPES = {
    HdPrimTypeTokens->mesh,
};

const TfTokenVector Hd_RUZINO_RenderDelegate::SUPPORTED_SPRIM_TYPES = {
    HdPrimTypeTokens->camera,      HdPrimTypeTokens->simpleLight,
    HdPrimTypeTokens->sphereLight, HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->material,
};

const TfTokenVector Hd_RUZINO_RenderDelegate::SUPPORTED_BPRIM_TYPES = {
    HdPrimTypeTokens->renderBuffer,
};

Hd_RUZINO_RenderDelegate::Hd_RUZINO_RenderDelegate() : HdRenderDelegate()
{
    _Initialize();
}

Hd_RUZINO_RenderDelegate::Hd_RUZINO_RenderDelegate(
    const HdRenderSettingsMap& settingsMap)
    : HdRenderDelegate(settingsMap)
{
    _Initialize();
}

static void _RenderCallback(
    Hd_RUZINO_Renderer* renderer,
    HdRenderThread* renderThread)
{
    renderer->Clear();
    renderer->Render(renderThread);
}

std::mutex Hd_RUZINO_RenderDelegate::_mutexResourceRegistry;
std::atomic_int Hd_RUZINO_RenderDelegate::_counterResourceRegistry;
HdResourceRegistrySharedPtr Hd_RUZINO_RenderDelegate::_resourceRegistry;

void Hd_RUZINO_RenderDelegate::_Initialize()
{
    // Initialize the settings and settings descriptors.
    _settingDescriptors.resize(5);
    _settingDescriptors[0] = {
        "Enable Scene Colors",
        HdEmbreeRenderSettingsTokens->enableSceneColors,
        VtValue(HdEmbreeConfig::GetInstance().useFaceColors)
    };
    _settingDescriptors[1] = {
        "Enable Ambient Occlusion",
        HdEmbreeRenderSettingsTokens->enableAmbientOcclusion,
        VtValue(HdEmbreeConfig::GetInstance().ambientOcclusionSamples > 0)
    };
    _settingDescriptors[2] = {
        "Ambient Occlusion Samples",
        HdEmbreeRenderSettingsTokens->ambientOcclusionSamples,
        VtValue(
            static_cast<int>(
                HdEmbreeConfig::GetInstance().ambientOcclusionSamples))
    };
    _settingDescriptors[3] = {
        "Samples To Convergence",
        HdRenderSettingsTokens->convergedSamplesPerPixel,
        VtValue(
            static_cast<int>(
                HdEmbreeConfig::GetInstance().samplesToConvergence))
    };

    _settingDescriptors[4] = { "Render Mode",
                               HdEmbreeRenderSettingsTokens->renderMode,
                               VtValue(0) };
    _PopulateDefaultSettings(_settingDescriptors);

    RenderGlobalPayloadGL global_payload =
        RenderGlobalPayloadGL(&cameras, &lights, &meshes, &materials);

    std::unique_ptr<NodeTreeExecutor> render_executor =
        std::make_unique<EagerNodeTreeExecutorRender>();

    node_system = create_dynamic_loading_system();
    node_system->load_configuration("gl_based_render_nodes.json");

    namespace fs = std::filesystem;
    std::regex submission_suffix(R"(.*_nodes_hw_submissions_render\.json)");
    spdlog::info("LOADING SUBMISSIONS [Render]");
    for (auto& itr : fs::directory_iterator(".")) {
        if (std::regex_match(itr.path().string(), submission_suffix)) {
            spdlog::info("Found: %s", itr.path().string().c_str());
            node_system->load_configuration(itr.path().string());
        }
    }

    node_system->set_node_tree_executor(std::move(render_executor));
    node_system->allow_ui_execution = false;
    node_system->init();

    node_system->set_global_params(global_payload);

    _renderParam = std::make_shared<Hd_RUZINO_RenderParam>(
        &_renderThread, &_sceneVersion, &lights, &cameras, &meshes, &materials);
    _renderParam->node_system = node_system.get();

    _renderer = std::make_shared<Hd_RUZINO_Renderer>(_renderParam.get());

    // Set the background render thread's rendering entrypoint to
    // HdEmbreeRenderer::Render.
    _renderThread.SetRenderCallback(
        std::bind(_RenderCallback, _renderer.get(), &_renderThread));
    _renderThread.StartThread();

    // Initialize one resource registry for all embree plugins
    std::lock_guard<std::mutex> guard(_mutexResourceRegistry);

    if (_counterResourceRegistry.fetch_add(1) == 0) {
        _resourceRegistry = std::make_shared<HdResourceRegistry>();
    }
    _resourceRegistry = std::make_shared<HdResourceRegistry>();
}

HdAovDescriptor Hd_RUZINO_RenderDelegate::GetDefaultAovDescriptor(
    const TfToken& name) const
{
    if (name == HdAovTokens->color) {
        return HdAovDescriptor(
            HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
    }
    if (name == HdAovTokens->normal || name == HdAovTokens->Neye) {
        return HdAovDescriptor(
            HdFormatFloat32Vec3, false, VtValue(GfVec3f(-1.0f)));
    }
    // if (name == HdAovTokens->depth) {
    //     return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
    // }
    if (name == HdAovTokens->cameraDepth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    }
    // if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
    //     name == HdAovTokens->elementId) {
    //     return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    // }
    HdParsedAovToken aovId(name);
    if (aovId.isPrimvar) {
        return HdAovDescriptor(
            HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    }

    return HdAovDescriptor();
}

Hd_RUZINO_RenderDelegate::~Hd_RUZINO_RenderDelegate()
{
    if (_renderThread.IsThreadRunning()) {
        _renderThread.StopRender();
        _renderThread.StopThread();
    }

    _resourceRegistry.reset();
    std::cout << "Destroying Tiny RenderDelegate" << std::endl;
}

const TfTokenVector& Hd_RUZINO_RenderDelegate::GetSupportedRprimTypes() const
{
    return SUPPORTED_RPRIM_TYPES;
}

const TfTokenVector& Hd_RUZINO_RenderDelegate::GetSupportedSprimTypes() const
{
    return SUPPORTED_SPRIM_TYPES;
}

const TfTokenVector& Hd_RUZINO_RenderDelegate::GetSupportedBprimTypes() const
{
    return SUPPORTED_BPRIM_TYPES;
}

HdResourceRegistrySharedPtr Hd_RUZINO_RenderDelegate::GetResourceRegistry()
    const
{
    return _resourceRegistry;
}

void Hd_RUZINO_RenderDelegate::CommitResources(HdChangeTracker* tracker)
{
}

HdRenderPassSharedPtr Hd_RUZINO_RenderDelegate::CreateRenderPass(
    HdRenderIndex* index,
    const HdRprimCollection& collection)
{
    std::cout << "Create RenderPass with Collection=" << collection.GetName()
              << std::endl;

    return std::make_shared<Hd_RUZINO_RenderPass>(
        index, collection, &_renderThread, _renderer.get(), &_sceneVersion);
}

HdRprim* Hd_RUZINO_RenderDelegate::CreateRprim(
    const TfToken& typeId,
    const SdfPath& rprimId)
{
    std::cout << "Create Rprim type=" << typeId.GetText() << " id=" << rprimId
              << std::endl;

    if (typeId == HdPrimTypeTokens->mesh) {
        auto mesh = new Hd_RUZINO_Mesh(rprimId);
        meshes.push_back(mesh);
        return mesh;
    }
    TF_CODING_ERROR(
        "Unknown Rprim type=%s id=%s", typeId.GetText(), rprimId.GetText());
    return nullptr;
}

void Hd_RUZINO_RenderDelegate::DestroyRprim(HdRprim* rPrim)
{
    spdlog::info("Destroy Tiny Rprim id=" + rPrim->GetId().GetString());
    meshes.erase(
        std::remove(meshes.begin(), meshes.end(), rPrim), meshes.end());
    delete rPrim;
}

HdSprim* Hd_RUZINO_RenderDelegate::CreateSprim(
    const TfToken& typeId,
    const SdfPath& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        auto camera = new Hd_RUZINO_Camera(sprimId);
        cameras.push_back(camera);
        return camera;
    }
    else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(sprimId);
    }
    else if (typeId == HdPrimTypeTokens->material) {
        auto material = new Hd_RUZINO_Material(sprimId);
        materials[sprimId] = material;
        return material;
    }
    else if (
        typeId == HdPrimTypeTokens->simpleLight ||
        typeId == HdPrimTypeTokens->sphereLight) {
        auto light = new Hd_RUZINO_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->domeLight) {
        auto light = new Hd_RUZINO_Dome_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

HdSprim* Hd_RUZINO_RenderDelegate::CreateFallbackSprim(const TfToken& typeId)
{
    // For fallback sprims, create objects with an empty scene path.
    // They'll use default values and won't be updated by a scene delegate.
    if (typeId == HdPrimTypeTokens->camera) {
        auto camera = new Hd_RUZINO_Camera(SdfPath::EmptyPath());
        cameras.push_back(camera);
        return camera;
    }
    else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(SdfPath::EmptyPath());
    }
    else if (typeId == HdPrimTypeTokens->material) {
        auto material = new Hd_RUZINO_Material(SdfPath::EmptyPath());
        materials[SdfPath::EmptyPath()] = material;
        return material;
    }
    else if (
        typeId == HdPrimTypeTokens->simpleLight ||
        typeId == HdPrimTypeTokens->sphereLight) {
        auto light = new Hd_RUZINO_Light(SdfPath::EmptyPath(), typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->domeLight) {
        auto light = new Hd_RUZINO_Dome_Light(SdfPath::EmptyPath(), typeId);
        lights.push_back(light);
        return light;
    }
    else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

void Hd_RUZINO_RenderDelegate::DestroySprim(HdSprim* sPrim)
{
    lights.erase(
        std::remove(lights.begin(), lights.end(), sPrim), lights.end());
    cameras.erase(
        std::remove(cameras.begin(), cameras.end(), sPrim), cameras.end());
    materials.erase(sPrim->GetId());
    delete sPrim;
}

HdBprim* Hd_RUZINO_RenderDelegate::CreateBprim(
    const TfToken& typeId,
    const SdfPath& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        spdlog::info(
            "Create bprim: type id=" + typeId.GetString() +
            ",prim id = " + bprimId.GetString());

        return new Hd_RUZINO_RenderBufferGL(bprimId);
    }
    TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    return nullptr;
}

HdBprim* Hd_RUZINO_RenderDelegate::CreateFallbackBprim(const TfToken& typeId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new Hd_RUZINO_RenderBufferGL(SdfPath::EmptyPath());
    }
    TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    return nullptr;
}

void Hd_RUZINO_RenderDelegate::DestroyBprim(HdBprim* bPrim)
{
    std::string sentence = "Destroy Bprim";
    auto bprim_name = bPrim->GetId().GetString();
    if (!bprim_name.empty()) {
        sentence += " id=" + bprim_name;
    }
    spdlog::info(sentence);
    delete bPrim;
}

HdInstancer* Hd_RUZINO_RenderDelegate::CreateInstancer(
    HdSceneDelegate* delegate,
    const SdfPath& id)
{
    return new HdEmbreeInstancer(delegate, id);
}

void Hd_RUZINO_RenderDelegate::DestroyInstancer(HdInstancer* instancer)
{
    TF_CODING_ERROR("Destroy instancer not supported");
}

HdRenderParam* Hd_RUZINO_RenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

VtValue Hd_RUZINO_RenderDelegate::GetRenderSetting(TfToken const& key) const
{
    if (key == TfToken("RenderNodeSystem")) {
        return VtValue(reinterpret_cast<const void*>(&node_system));
    }

    return HdRenderDelegate::GetRenderSetting(key);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
