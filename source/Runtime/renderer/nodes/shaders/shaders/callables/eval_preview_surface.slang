import callable_data;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;
import Scene.UsdPreviewSurface;
import pbrlib.genslang.mx_roughness_anisotropy;
import pbrlib.genslang.lib.mx_microfacet;
import pbrlib.genslang.lib.mx_microfacet_specular;
import pbrlib.genslang.lib.mx_microfacet_diffuse;
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;
#include "utils/Math/MathConstants.slangh"

float eval_preview_surface_pdf(
    VertexData vd,
    float3 L,
    float3 V,
    float3 diffuseColor,
    float3 specularColor,
    float metallic,
    float roughness,
    float opacity,
    float ior,
    uint eta_flipped)
{
    if (opacity < 0.25f) {
        return 1.0f;
    }

    bool valid;
    ShadingFrame sf =
        ShadingFrame.createSafe(vd.normalWorld, float4(1, 0, 0, 1), valid);
    float3 V_local = sf.toLocal(V);
    float3 L_local = sf.toLocal(normalize(L));
    float NdotV = clamp(V_local.z, M_FLOAT_EPS, 1.0);

    float eta = eta_flipped != 0 ? (1.0 / ior) : ior;
    float fresnel = fresnel_dielectric(NdotV, eta);

    bool isSpecularWorkflow = false;
    float3 final_diffuse_color;
    float3 final_specular_color;
    float final_metallic;

    if (isSpecularWorkflow) {
        final_diffuse_color = diffuseColor;
        final_specular_color = specularColor;
        final_metallic = 0.0;
    }
    else {
        final_metallic = metallic;
        final_diffuse_color = diffuseColor * (1.0 - metallic);
        final_specular_color = lerp(float3(0.04), diffuseColor, metallic);
    }

    float metal_weight = final_metallic;
    float nonmetal_weight = 1.0 - final_metallic;
    float diffuse_weight = (1.0 - fresnel) * luminance(final_diffuse_color);
    float reflection_weight = fresnel;

    float total_nonmetal_weight = diffuse_weight + reflection_weight;
    if (total_nonmetal_weight > M_FLOAT_EPS) {
        diffuse_weight /= total_nonmetal_weight;
        reflection_weight /= total_nonmetal_weight;
    }

    float metal_pdf = 0.0;
    float nonmetal_pdf = 0.0;

    if (L_local.z > -M_FLOAT_EPS) {
        float3 h_unnormalized = V_local + L_local;
        float h_len = length(h_unnormalized);
        if (h_len > M_FLOAT_EPS) {
            float3 H = h_unnormalized / h_len;
            float VdotH = max(dot(V_local, H), M_FLOAT_EPS);
            float2 alpha = float2(roughness * roughness, roughness * roughness);
            float D = mx_ggx_NDF(H, alpha);
            float G1 = mx_ggx_smith_G1_aniso(
                NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
            float vndf_pdf = D * G1 * VdotH / NdotV;
            metal_pdf = vndf_pdf / (4.0 * VdotH);
        }
    }

    if (total_nonmetal_weight > M_FLOAT_EPS && L_local.z > -M_FLOAT_EPS) {
        float diffuse_pdf = max(L_local.z, 0.0) * M_1_PI;
        float reflection_pdf = 0.0;

        float3 h_unnormalized = V_local + L_local;
        float h_len = length(h_unnormalized);
        if (h_len > M_FLOAT_EPS) {
            float3 H = h_unnormalized / h_len;
            float VdotH = max(dot(V_local, H), M_FLOAT_EPS);
            float2 alpha = float2(roughness * roughness, roughness * roughness);
            float D = mx_ggx_NDF(H, alpha);
            float G1 = mx_ggx_smith_G1_aniso(
                NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
            float vndf_pdf = D * G1 * VdotH / NdotV;
            reflection_pdf = vndf_pdf / (4.0 * VdotH);
        }

        nonmetal_pdf =
            diffuse_pdf * diffuse_weight + reflection_pdf * reflection_weight;
    }

    float pdf = metal_pdf * metal_weight + nonmetal_pdf * nonmetal_weight;
    return max(pdf, M_FLOAT_EPS);
}

[shader("callable")]
void eval_preview_surface(inout EvalCallableData data)
{
    MaterialParams params_blob =
        materialParamsBuffer[data.material_params_index];
    PreviewSurfaceMaterialParams params;
    params = reinterpret<PreviewSurfaceMaterialParams>(params_blob);

    const float MIN_ROUGHNESS = 0.001;
    params.roughness = max(params.roughness, MIN_ROUGHNESS);
    params.clearcoatRoughness = max(params.clearcoatRoughness, MIN_ROUGHNESS);

    VertexData vd;
    vd.normalWorld = data.vertexInfo.normalW;
    vd.tangentWorld = data.vertexInfo.tangentW.xyz;
    vd.bitangentWorld =
        cross(data.vertexInfo.normalW, data.vertexInfo.tangentW.xyz) *
        data.vertexInfo.tangentW.w;
    vd.positionWorld = data.vertexInfo.posW;

    surfaceshader bsdf_result = surfaceshader(float3(0.0), float3(0.0));
    IMP_UsdPreviewSurface_surfaceshader(
        vd,
        data.L,
        data.V,
        data.eta_flipped,
        params.diffuseColor,
        params.emissiveColor,
        params.useSpecularWorkflow,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.clearcoat,
        params.clearcoatRoughness,
        params.opacity,
        params.opacityMode,
        params.opacityThreshold,
        params.ior,
        params.normal,
        params.displacement,
        params.occlusion,
        bsdf_result);

    data.color = Spectrum(bsdf_result.color);
    data.evalPdf = eval_preview_surface_pdf(
        vd,
        data.L,
        data.V,
        params.diffuseColor,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.opacity,
        params.ior,
        data.eta_flipped);

    data.sampledDir = sample_preview_surface(
        vd,
        data.V,
        params.diffuseColor,
        params.emissiveColor,
        params.useSpecularWorkflow,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.clearcoat,
        params.clearcoatRoughness,
        params.opacity,
        params.opacityMode,
        params.opacityThreshold,
        params.ior,
        params.normal,
        params.displacement,
        params.occlusion,
        data.eta_flipped,
        data.seed,
        data.pdf);

    surfaceshader weight_result = surfaceshader(float3(0.0), float3(0.0));
    IMP_UsdPreviewSurface_surfaceshader(
        vd,
        data.sampledDir,
        data.V,
        data.eta_flipped,
        params.diffuseColor,
        params.emissiveColor,
        params.useSpecularWorkflow,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.clearcoat,
        params.clearcoatRoughness,
        params.opacity,
        params.opacityMode,
        params.opacityThreshold,
        params.ior,
        params.normal,
        params.displacement,
        params.occlusion,
        weight_result);

    data.throughput = Spectrum(weight_result.color);
}
