import callable_data;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;
import Scene.StandardSurface;
import pbrlib.genslang.mx_roughness_anisotropy;
import pbrlib.genslang.mx_sheen_bsdf;
import stdlib.genslang.mx_luminance_color3;
import stdlib.genslang.mx_rotate_vector3;
import pbrlib.genslang.lib.mx_microfacet;
import pbrlib.genslang.lib.mx_microfacet_specular;
import pbrlib.genslang.lib.mx_microfacet_diffuse;
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;
#include "utils/Math/MathConstants.slangh"

float eval_standard_surface_pdf(
    VertexData vd,
    float3 L,
    float3 V,
    float base,
    float3 base_color,
    float diffuse_roughness,
    float metalness,
    float specular,
    float3 specular_color,
    float specular_roughness,
    float specular_IOR,
    float specular_anisotropy,
    float specular_rotation,
    float transmission,
    float3 transmission_color,
    float transmission_depth,
    float3 transmission_scatter,
    float transmission_scatter_anisotropy,
    float transmission_dispersion,
    float transmission_extra_roughness,
    float subsurface,
    float3 subsurface_color,
    float3 subsurface_radius,
    float subsurface_scale,
    float subsurface_anisotropy,
    float sheen,
    float3 sheen_color,
    float sheen_roughness,
    float coat,
    float3 coat_color,
    float coat_roughness,
    float coat_anisotropy,
    float coat_rotation,
    float coat_IOR,
    float3 coat_normal,
    float coat_affect_color,
    float coat_affect_roughness,
    float thin_film_thickness,
    float thin_film_IOR,
    float emission,
    float3 emission_color,
    float3 opacity,
    bool thin_walled,
    float3 normal,
    float3 tangent,
    uint eta_flipped)
{
    if (transmission > 0.0 && eta_flipped > 0.0) {
        specular_IOR = 1.0 / specular_IOR;
    }

    if (opacity.r < 0.25f) {
        return 1.0f;
    }

    float3 shading_normal = normal;
    float3 shading_tangent = tangent;
    if (dot(V, normal) < 0.0) {
        shading_normal = -normal;
        shading_tangent = -tangent;
    }

    float3 coat_shading_normal = coat_normal;
    float3 coat_shading_tangent = tangent;
    if (dot(V, coat_normal) < 0.0) {
        coat_shading_normal = -coat_normal;
        coat_shading_tangent = -tangent;
    }

    float3 rotated_tangent;
    float rotation_angle = specular_rotation * 360.0;
    mx_rotate_vector3(
        shading_tangent, rotation_angle, shading_normal, rotated_tangent);

    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        shading_normal, float4(rotated_tangent, 1.0), valid);

    float3 V_local = sf.toLocal(V);
    float3 L_local = sf.toLocal(normalize(L));
    float NdotV = clamp(V_local.z, M_FLOAT_EPS, 1.0);

    float2 alpha;
    mx_roughness_anisotropy(specular_roughness, specular_anisotropy, alpha);

    float3 coat_rotated_tangent;
    float coat_rotation_angle = coat_rotation * 360.0;
    mx_rotate_vector3(
        coat_shading_tangent,
        coat_rotation_angle,
        coat_shading_normal,
        coat_rotated_tangent);
    coat_rotated_tangent = normalize(coat_rotated_tangent);

    bool coat_valid;
    ShadingFrame coat_sf = ShadingFrame.createSafe(
        coat_shading_normal, float4(coat_rotated_tangent, 1.0), coat_valid);
    float3 V_coat_local = coat_sf.toLocal(V);
    float3 L_world = sf.fromLocal(L_local);
    float3 L_coat_local = coat_sf.toLocal(L_world);
    float coat_NdotV = clamp(V_coat_local.z, M_FLOAT_EPS, 1.0);

    float coat_weight = clamp(coat, 0.0, 1.0);
    float2 coat_alpha;
    mx_roughness_anisotropy(coat_roughness, coat_anisotropy, coat_alpha);
    float coat_fresnel = fresnel_dielectric(coat_NdotV, coat_IOR);

    float eta = specular_IOR;
    float base_fresnel = fresnel_dielectric(NdotV, eta);

    float coat_reflection_prob = coat_weight * coat_fresnel;
    float coat_sample_weight = coat_reflection_prob;
    float base_sample_weight = 1.0 - coat_sample_weight;

    float metal_weight = metalness;
    float nonmetal_weight = 1.0 - metalness;
    float diffuse_weight = base * (1.0 - base_fresnel) * luminance(base_color) *
                           (1 - transmission);
    float reflection_weight = base_fresnel * specular;
    float transmission_weight =
        (1.0 - base_fresnel) * transmission * luminance(transmission_color);

    float total_nonmetal_weight =
        diffuse_weight + reflection_weight + transmission_weight;
    if (total_nonmetal_weight > M_FLOAT_EPS) {
        diffuse_weight /= total_nonmetal_weight;
        reflection_weight /= total_nonmetal_weight;
        transmission_weight /= total_nonmetal_weight;
    }

    float coat_pdf = 0.0;
    if (L_coat_local.z > -M_FLOAT_EPS) {
        float3 h_coat_unnormalized = V_coat_local + L_coat_local;
        float h_coat_len = length(h_coat_unnormalized);
        if (h_coat_len > M_FLOAT_EPS) {
            float3 H_coat = h_coat_unnormalized / h_coat_len;
            float VdotH_coat = max(dot(V_coat_local, H_coat), M_FLOAT_EPS);

            float D = mx_ggx_NDF(H_coat, coat_alpha);
            float G1 = mx_ggx_smith_G1_aniso(
                coat_NdotV,
                V_coat_local.x,
                V_coat_local.y,
                coat_alpha.x,
                coat_alpha.y);
            float vndf_pdf = D * G1 * VdotH_coat / coat_NdotV;
            coat_pdf = vndf_pdf / (4.0 * VdotH_coat);
        }
    }

    float metal_pdf = 0.0;
    float nonmetal_pdf = 0.0;

    if (L_local.z > -M_FLOAT_EPS) {
        float3 h_unnormalized = V_local + L_local;
        float h_len = length(h_unnormalized);
        if (h_len > M_FLOAT_EPS) {
            float3 H = h_unnormalized / h_len;
            float VdotH = max(dot(V_local, H), M_FLOAT_EPS);

            float D = mx_ggx_NDF(H, alpha);
            float G1 = mx_ggx_smith_G1_aniso(
                NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
            float vndf_pdf = D * G1 * VdotH / NdotV;
            metal_pdf = vndf_pdf / (4.0 * VdotH);
        }
    }

    if (total_nonmetal_weight > M_FLOAT_EPS) {
        float diffuse_pdf = 0.0;
        float reflection_pdf = 0.0;
        float transmission_pdf = 0.0;

        if (L_local.z > -M_FLOAT_EPS) {
            diffuse_pdf = max(L_local.z, 0.0) * M_1_PI;

            float3 h_unnormalized = V_local + L_local;
            float h_len = length(h_unnormalized);
            if (h_len > M_FLOAT_EPS) {
                float3 H = h_unnormalized / h_len;
                float VdotH = max(dot(V_local, H), M_FLOAT_EPS);

                float D = mx_ggx_NDF(H, alpha);
                float G1 = mx_ggx_smith_G1_aniso(
                    NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
                float vndf_pdf = D * G1 * VdotH / NdotV;
                reflection_pdf = vndf_pdf / (4.0 * VdotH);
            }
        }
        else if (L_local.z < -M_FLOAT_EPS) {
            float3 h_unnormalized = V_local + L_local * eta;
            float h_len = length(h_unnormalized);
            if (h_len > M_FLOAT_EPS) {
                float3 H = h_unnormalized / h_len;
                if (H.z < 0.0) {
                    H = -H;
                }

                float VdotH = max(dot(V_local, H), M_FLOAT_EPS);
                float LdotH = max(abs(dot(L_local, H)), M_FLOAT_EPS);

                float D = mx_ggx_NDF(H, alpha);
                float G1 = mx_ggx_smith_G1_aniso(
                    NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
                float vndf_pdf = D * G1 * VdotH / NdotV;

                float denom = VdotH + LdotH / eta;
                float jacobian = (eta * eta * LdotH) / (denom * denom);
                transmission_pdf = vndf_pdf * jacobian / (4.0 * VdotH);
            }
        }

        nonmetal_pdf = diffuse_pdf * diffuse_weight +
                       reflection_pdf * reflection_weight +
                       transmission_pdf * transmission_weight;
    }

    float base_pdf = metal_pdf * metal_weight + nonmetal_pdf * nonmetal_weight;
    float pdf = coat_pdf * coat_sample_weight + base_pdf * base_sample_weight;
    return max(pdf, M_FLOAT_EPS);
}

[shader("callable")]
void eval_standard_surface(inout EvalCallableData data)
{
    MaterialParams params_blob =
        materialParamsBuffer[data.material_params_index];
    PackedStandardSurfaceMaterialParams pakced_params;
    pakced_params =
        reinterpret<PackedStandardSurfaceMaterialParams>(params_blob);

    StandardSurfaceMaterialParams params =
        unpackStandardSurfaceMaterialParams(pakced_params);

    const float MIN_ROUGHNESS = 0.001f;
    params.specular_roughness = max(params.specular_roughness, MIN_ROUGHNESS);
    params.coat_roughness = max(params.coat_roughness, MIN_ROUGHNESS);

    VertexData vd;
    vd.normalWorld = data.vertexInfo.normalW;
    vd.tangentWorld = data.vertexInfo.tangentW.xyz;
    vd.positionWorld = data.vertexInfo.posW;

    surfaceshader bsdf_result = surfaceshader(float3(0.0), float3(0.0));
    NG_standard_surface_surfaceshader_100(
        vd,
        data.L,
        data.V,
        data.eta_flipped,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        bsdf_result);

    data.color = Spectrum(bsdf_result.color);
    data.evalPdf = eval_standard_surface_pdf(
        vd,
        data.L,
        data.V,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        data.eta_flipped);

    data.sampledDir = sample_standard_surface(
        vd,
        data.V,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        data.eta_flipped,
        data.seed,
        data.pdf);

    surfaceshader weight_result = surfaceshader(float3(0.0), float3(0.0));
    NG_standard_surface_surfaceshader_100(
        vd,
        data.sampledDir,
        data.V,
        data.eta_flipped,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        weight_result);

    data.throughput = Spectrum(weight_result.color);
}
