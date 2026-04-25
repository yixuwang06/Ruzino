import callable_data;
#include "utils/random.slangh"
#include "utils/ray.slang"
import Scene.SceneTypes;
import Scene.BindlessMaterial;
import Scene.BindlessVertexBuffer;
import Scene.Lights.LightData;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;
import utils.Spectrum;

RaytracingAccelerationStructure SceneBVH : register(t0);
StructuredBuffer<uint2> inPixelTarget;
StructuredBuffer<RayInfo> rays;
RWStructuredBuffer<uint> random_seeds;
RWTexture2D<float4> output;

ConstantBuffer<HW7PathTracingConstants> hw7Constants;
StructuredBuffer<LightData> lightBuffer;

struct HW7PathTracingConstants {
    uint lightCount;
    uint materialFetchCallableBaseIndex;
    uint maxDepth;
    uint rrStartDepth;
    float rrProbFloor;
    uint debugView;
};

struct RayPayload {
    uint hit;
    uint seed;
    uint materialID;
    uint etaFlipped;
    VertexInfo vertex;
};

struct ShadowPayload {
    uint occluded;
};

static const float kPi = 3.14159265359f;

float power_heuristic(float pdfA, float pdfB)
{
    float a2 = pdfA * pdfA;
    float b2 = pdfB * pdfB;
    return a2 / max(a2 + b2, 1e-12f);
}

bool find_rect_light(out LightData light)
{
    for (uint i = 0; i < hw7Constants.lightCount; ++i) {
        if (lightBuffer[i].type == uint(LightType::Rect)) {
            light = lightBuffer[i];
            return true;
        }
    }

    light = LightData();
    return false;
}

bool trace_visibility(float3 origin, float3 dir, float tMax)
{
    ShadowPayload shadowPayload;
    shadowPayload.occluded = 0;

    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = dir;
    shadowRay.TMin = 1e-3f;
    shadowRay.TMax = max(tMax - 2e-3f, shadowRay.TMin);

    TraceRay(
        SceneBVH,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF,
        1,
        1,
        1,
        shadowRay,
        shadowPayload);

    return shadowPayload.occluded == 0;
}

float3 sample_rect_light_point(LightData light, float2 xi)
{
    return light.posW + (xi.x - 0.5f) * light.tangent +
           (xi.y - 0.5f) * light.bitangent;
}

bool intersect_rect_light_ray(
    float3 origin,
    float3 dir,
    LightData light,
    out float t,
    out float cosLight,
    out float lightPdfW)
{
    t = 0.0f;
    cosLight = 0.0f;
    lightPdfW = 0.0f;

    float denom = dot(dir, light.dirW);
    if (denom >= -1e-5f) {
        return false;
    }

    t = dot(light.posW - origin, light.dirW) / denom;
    if (t <= 1e-3f) {
        return false;
    }

    float3 hitPos = origin + dir * t;
    float3 local = hitPos - light.posW;
    float tangentLen2 = max(dot(light.tangent, light.tangent), 1e-6f);
    float bitangentLen2 = max(dot(light.bitangent, light.bitangent), 1e-6f);
    float u = dot(local, light.tangent) / tangentLen2;
    float v = dot(local, light.bitangent) / bitangentLen2;

    if (abs(u) > 0.5f || abs(v) > 0.5f) {
        return false;
    }

    cosLight = max(dot(light.dirW, -dir), 0.0f);
    if (cosLight <= 0.0f) {
        return false;
    }

    float pdfA = 1.0f / max(light.surfaceArea, 1e-6f);
    float dist2 = t * t;
    lightPdfW = dist2 * pdfA / max(cosLight, 1e-6f);
    return true;
}

float3 sky_color(float3 dir)
{
    float t = 0.5f * (dir.z + 1.0f);
    return lerp(float3(0.15f, 0.18f, 0.22f), float3(0.4f, 0.45f, 0.55f), t);
}

bool find_dome_light(out LightData light)
{
    for (uint i = 0; i < hw7Constants.lightCount; ++i) {
        if (lightBuffer[i].type == uint(LightType::Dome)) {
            light = lightBuffer[i];
            return true;
        }
    }

    light = LightData();
    return false;
}

[shader("raygeneration")]
void RayGen()
{
    uint launchIndex = DispatchRaysIndex().x;
    uint2 pixel = inPixelTarget[launchIndex];
    RayInfo ray = rays[launchIndex];
    uint seed = random_seeds[launchIndex];

    Spectrum accumulated = Spectrum(0.0);
    Spectrum throughput = Spectrum(1.0);

    RayDesc currentRay = get_ray_desc(ray);
    currentRay.TMax = 50000.0f;

    for (uint depth = 0; depth < hw7Constants.maxDepth; ++depth) {
        RayPayload payload;
        payload.hit = 0;
        payload.seed = seed;
        payload.materialID = uint(-1);
        payload.etaFlipped = 0;

        TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 1, 0, currentRay, payload);
        seed = payload.seed;

        if (payload.hit == 0) {
            Spectrum envColor = Spectrum(0.0);
            LightData domeLight;
            if (find_dome_light(domeLight)) {
                float3 localDir =
                    mul(float4(currentRay.Direction, 0.0f), domeLight.transMat)
                        .xyz;
                localDir = normalize(localDir);

                if (domeLight.textureIndex >= 0) {
                    float phi = atan2(localDir.z, localDir.x);
                    float theta = acos(clamp(localDir.y, -1.0, 1.0));
                    float u = (phi + kPi) / (2.0 * kPi);
                    float v = theta / kPi;

                    float4 texColor =
                        t_BindlessTextures[domeLight.textureIndex].SampleLevel(
                            samplers[1], float2(u, v), 0);
                    envColor =
                        Spectrum(texColor.rgb * domeLight.intensity);
                }
                else {
                    envColor = Spectrum(domeLight.intensity);
                }
            }
            else {
                envColor = Spectrum(float3(0.0f, 0.0f, 0.0f));
            }

            accumulated = add(accumulated, mul(throughput, envColor));
            break;
        }

        VertexInfo vertex = payload.vertex;
        float3 P = vertex.posW;
        float3 L = vertex.normalW;
        float lightWeight = 0.0f;
        bool lightVisible = false;

        if (hw7Constants.debugView == 1 && depth == 0) {
            output[pixel] = float4(abs(vertex.normalW), 1.0f);
            random_seeds[launchIndex] = seed;
            return;
        }

        LightData rectLight;
        if (find_rect_light(rectLight)) {
            float2 lightXi = random_float2(seed);
            float3 lightPos = sample_rect_light_point(rectLight, lightXi);
            float3 toLight = lightPos - P;
            float dist2 = dot(toLight, toLight);
            if (dist2 > 1e-6f) {
                float dist = sqrt(dist2);
                L = toLight / dist;
                float cosLight = max(dot(rectLight.dirW, -L), 0.0f);
                if (cosLight > 0.0f) {
                    ShadowPayload shadowPayload;
                    shadowPayload.occluded = 0;

                    RayDesc shadowRay;
                    shadowRay.Origin = P + vertex.faceNormalW * 1e-3f;
                    shadowRay.Direction = L;
                    shadowRay.TMin = 1e-3f;
                    shadowRay.TMax = dist - 2e-3f;

                    TraceRay(
                        SceneBVH,
                        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                        0xFF,
                        1,
                        1,
                        1,
                        shadowRay,
                        shadowPayload);

                    if (shadowPayload.occluded == 0) {
                        float pdfA = 1.0f / max(rectLight.surfaceArea, 1e-6f);
                        lightWeight = cosLight / max(dist2 * pdfA, 1e-6f);
                        lightVisible = true;
                    }
                }
            }
        }

        if (payload.materialID == uint(-1)) {
            float3 fallback = float3(0.8f, 0.8f, 0.8f);
            if (lightVisible) {
                accumulated = add(
                    accumulated,
                    mul(
                        throughput,
                        Spectrum(fallback * rectLight.intensity * lightWeight)));
            }
            break;
        }

        MaterialHeader materialHeader = materialHeaderBuffer[payload.materialID];

        FetchCallableData fetchData;
        fetchData.materialBlobID = materialHeader.material_blob_id;
        fetchData.material_params_index = launchIndex;
        fetchData.shader_type_id = 2;
        fetchData.vertexInfo = vertex;
        CallShader(
            hw7Constants.materialFetchCallableBaseIndex +
                materialHeader.material_type_id,
            fetchData);

        EvalCallableData evalData;
        evalData.color = Spectrum(0.0);
        evalData.throughput = Spectrum(1.0);
        evalData.L = L;
        evalData.V = -currentRay.Direction;
        evalData.sampledDir = vertex.normalW;
        evalData.pdf = 0.0f;
        evalData.lightPdf = 0.0f;
        evalData.seed = seed;
        evalData.eta_flipped = payload.etaFlipped;
        evalData.material_params_index = launchIndex;
        evalData.vertexInfo = vertex;
        CallShader(fetchData.shader_type_id, evalData);
        seed = evalData.seed;

        if (hw7Constants.debugView == 3 && depth == 0) {
            output[pixel] =
                float4(max(evalData.color.toFloat3(), 0.0f), 1.0f);
            random_seeds[launchIndex] = seed;
            return;
        }

        float pdf = evalData.pdf;
        if (pdf <= 1e-6f) {
            break;
        }

        float nDotL = max(dot(vertex.normalW, L), 0.0f);
        float3 direct_debug = float3(0.0f, 0.0f, 0.0f);
        if (lightVisible && nDotL > 0.0f) {
            float pdfA = 1.0f / max(rectLight.surfaceArea, 1e-6f);
            float lightPdfW =
                1.0f / max(lightWeight, 1e-6f) / max(nDotL, 1e-6f) * nDotL;
            float misWeight =
                power_heuristic(lightPdfW, max(evalData.lightPdf, 0.0f));
            direct_debug =
                mul(
                    Spectrum(rectLight.intensity * lightWeight * nDotL),
                    evalData.color)
                    .toFloat3() *
                misWeight;
            accumulated = add(
                accumulated,
                mul(
                    throughput,
                    Spectrum(direct_debug)));
        }

        if (hw7Constants.debugView == 2 && depth == 0) {
            output[pixel] = float4(max(direct_debug, 0.0f), 1.0f);
            random_seeds[launchIndex] = seed;
            return;
        }

        bool isTransmission = dot(evalData.sampledDir, vertex.faceNormalW) < 0.0f;
        float3 offsetNormal =
            isTransmission ? -vertex.faceNormalW : vertex.faceNormalW;
        float3 bsdfOrigin = P + offsetNormal * 1e-3f;
        float3 bsdfDir = normalize(evalData.sampledDir);

        if (find_rect_light(rectLight)) {
            float lightHitT = 0.0f;
            float lightHitCos = 0.0f;
            float lightHitPdfW = 0.0f;
            if (intersect_rect_light_ray(
                    bsdfOrigin,
                    bsdfDir,
                    rectLight,
                    lightHitT,
                    lightHitCos,
                    lightHitPdfW) &&
                trace_visibility(bsdfOrigin, bsdfDir, lightHitT)) {
                float misWeight =
                    power_heuristic(max(pdf, 0.0f), lightHitPdfW);
                Spectrum bsdfToLight =
                    mul(div(evalData.throughput, max(pdf, 1e-6f)),
                        Spectrum(rectLight.intensity * misWeight));
                accumulated = add(accumulated, mul(throughput, bsdfToLight));

                if (hw7Constants.debugView == 2 && depth == 0) {
                    output[pixel] =
                        float4(max(bsdfToLight.toFloat3(), 0.0f), 1.0f);
                    random_seeds[launchIndex] = seed;
                    return;
                }

                break;
            }
        }

        if (any(isnan(evalData.throughput.toFloat3())) ||
            any(isinf(evalData.throughput.toFloat3())) ||
            any(isnan(evalData.sampledDir)) ||
            any(isinf(evalData.sampledDir))) {
            break;
        }

        if (depth >= hw7Constants.rrStartDepth) {
            float3 thr = throughput.toFloat3();
            float continueProb =
                min(0.95, max(hw7Constants.rrProbFloor, max(thr.x, max(thr.y, thr.z))));
            if (random_float(seed) > continueProb) {
                break;
            }
            throughput = div(throughput, continueProb);
        }

        throughput = mul(throughput, div(evalData.throughput, pdf));

        currentRay.Origin = P + offsetNormal * 1e-3f;
        currentRay.Direction = bsdfDir;
        currentRay.TMin = 1e-3f;
        currentRay.TMax = 50000.0f;
    }

    output[pixel] = float4(max(accumulated.toFloat3(), 0.0f), 1.0f);
    random_seeds[launchIndex] = seed;
}

[shader("closesthit")]
void ClosestHit(
    inout RayPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hit = 1;

    uint instanceId = InstanceID();
    uint primId = PrimitiveIndex();
    VertexInfo vertex = get_interpolated_vertex(
        instanceId,
        primId,
        float2(attribs.barycentrics.x, attribs.barycentrics.y));

    uint etaFlipped = 0;
    if (dot(WorldRayDirection(), vertex.normalW) >= 0.0f) {
        vertex.normalW = -vertex.normalW;
        vertex.tangentW = -vertex.tangentW;
        vertex.faceNormalW = -vertex.faceNormalW;
        etaFlipped = 1;
    }

    uint materialId = instanceDescBuffer[instanceId].materialID;
    if (vertex.subsetMaterialID != uint(-1)) {
        materialId = vertex.subsetMaterialID;
    }

    payload.vertex = vertex;
    payload.materialID = materialId;
    payload.etaFlipped = etaFlipped;
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.hit = 0;
}

[shader("closesthit")]
void ShadowHit(
    inout ShadowPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.occluded = 1;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.occluded = 0;
}
