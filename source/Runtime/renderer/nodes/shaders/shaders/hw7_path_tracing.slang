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
    uint materialOpacityCallableOffset;
    uint maxDepth;
    uint rrStartDepth;
    float rrProbFloor;
};

struct RayPayload {
    uint hit;
    uint seed;
    uint materialID;
    uint etaFlipped;
    float hitDistance;
    VertexInfo vertex;
};

struct ShadowPayload {
    uint occluded;
    uint recursionDepth;
    float rayTMax;
};

struct DirectLightSample {
    bool valid;
    uint lightIndex;
    float3 direction;
    float distance;
    float3 position;
    float3 radiance;
    float pdf;
};

struct RectLightHit {
    bool hit;
    uint lightIndex;
    float t;
    float3 position;
    float3 radiance;
    float pdf;
};

static const float kPi = 3.14159265359f;
// Favor a cleaner homework "beauty" render on complex scenes like Sponza by
// spending more work inside each accumulated sample instead of relying only on
// a higher external spp budget.
static const uint kPathsPerPixel = 4;
static const uint kPrimaryDirectLightSamples = 8;
static const uint kSecondaryDirectLightSamples = 2;

float power_heuristic(float pdfA, float pdfB)
{
    float a2 = pdfA * pdfA;
    float b2 = pdfB * pdfB;
    float denom = a2 + b2;
    return denom > 0.0f ? a2 / denom : 1.0f;
}

uint count_rect_lights()
{
    uint rectCount = 0;
    for (uint i = 0; i < hw7Constants.lightCount; ++i) {
        if (lightBuffer[i].type == uint(LightType::Rect)) {
            rectCount++;
        }
    }
    return rectCount;
}

bool get_rect_light_by_ordinal(
    uint ordinal,
    out uint lightIndex,
    out LightData light)
{
    uint rectCount = 0;
    for (uint i = 0; i < hw7Constants.lightCount; ++i) {
        if (lightBuffer[i].type != uint(LightType::Rect)) {
            continue;
        }

        if (rectCount == ordinal) {
            lightIndex = i;
            light = lightBuffer[i];
            return true;
        }
        rectCount++;
    }

    lightIndex = 0;
    light = LightData();
    return false;
}

float3 sample_rect_light_point(LightData light, float2 xi)
{
    return light.posW + (xi.x - 0.5f) * light.tangent +
           (xi.y - 0.5f) * light.bitangent;
}

bool trace_shadow_visibility(
    float3 origin,
    float3 offsetNormal,
    float3 direction,
    float maxDistance)
{
    ShadowPayload shadowPayload;
    shadowPayload.occluded = 0;
    shadowPayload.recursionDepth = 0;

    RayDesc shadowRay;
    shadowRay.Origin = origin + offsetNormal * 1e-3f;
    shadowRay.Direction = direction;
    shadowRay.TMin = 1e-3f;
    shadowRay.TMax = max(maxDistance - 2e-3f, 1e-3f);
    shadowPayload.rayTMax = shadowRay.TMax;

    TraceRay(
        SceneBVH,
        RAY_FLAG_NONE,
        0xFF,
        1,
        1,
        1,
        shadowRay,
        shadowPayload);

    return shadowPayload.occluded == 0;
}

DirectLightSample sample_rect_light(
    float3 hitPos,
    float3 geometricNormal,
    inout uint seed)
{
    DirectLightSample sample;
    sample.valid = false;
    sample.lightIndex = 0;
    sample.direction = geometricNormal;
    sample.distance = 0.0f;
    sample.position = hitPos;
    sample.radiance = float3(0.0f);
    sample.pdf = 0.0f;

    uint rectCount = count_rect_lights();
    if (rectCount == 0) {
        return sample;
    }

    uint ordinal =
        min(uint(random_float(seed) * float(rectCount)), rectCount - 1);
    uint lightIndex = 0;
    LightData light;
    if (!get_rect_light_by_ordinal(ordinal, lightIndex, light)) {
        return sample;
    }

    float3 lightPos = sample_rect_light_point(light, random_float2(seed));
    float3 toLight = lightPos - hitPos;
    float dist2 = dot(toLight, toLight);
    if (dist2 <= 1e-6f || light.surfaceArea <= 1e-6f) {
        return sample;
    }

    float distance = sqrt(dist2);
    float3 lightDir = toLight / distance;
    float cosLight = dot(light.dirW, -lightDir);
    if (cosLight <= 1e-6f) {
        return sample;
    }

    if (!trace_shadow_visibility(hitPos, geometricNormal, lightDir, distance)) {
        return sample;
    }

    float selectionPdf = 1.0f / float(rectCount);
    float solidAnglePdf = dist2 / max(light.surfaceArea * cosLight, 1e-6f);

    sample.valid = true;
    sample.lightIndex = lightIndex;
    sample.direction = lightDir;
    sample.distance = distance;
    sample.position = lightPos;
    sample.radiance = light.intensity;
    sample.pdf = selectionPdf * solidAnglePdf;
    return sample;
}

bool intersect_rect_light(
    LightData light,
    float3 rayOrigin,
    float3 rayDir,
    out float t,
    out float3 hitPos)
{
    float denom = dot(light.dirW, rayDir);
    if (abs(denom) <= 1e-6f) {
        t = 0.0f;
        hitPos = float3(0.0f);
        return false;
    }

    float tCandidate = dot(light.posW - rayOrigin, light.dirW) / denom;
    if (tCandidate <= 1e-3f) {
        t = 0.0f;
        hitPos = float3(0.0f);
        return false;
    }

    float3 candidatePos = rayOrigin + rayDir * tCandidate;
    float3 local = candidatePos - light.posW;
    float tangentDenom = dot(light.tangent, light.tangent);
    float bitangentDenom = dot(light.bitangent, light.bitangent);
    if (tangentDenom <= 1e-8f || bitangentDenom <= 1e-8f) {
        t = 0.0f;
        hitPos = float3(0.0f);
        return false;
    }

    float u = dot(local, light.tangent) / tangentDenom;
    float v = dot(local, light.bitangent) / bitangentDenom;
    if (abs(u) > 0.5f || abs(v) > 0.5f) {
        t = 0.0f;
        hitPos = float3(0.0f);
        return false;
    }

    t = tCandidate;
    hitPos = candidatePos;
    return true;
}

RectLightHit intersect_rect_lights(float3 rayOrigin, float3 rayDir)
{
    RectLightHit hit;
    hit.hit = false;
    hit.lightIndex = 0;
    hit.t = 0.0f;
    hit.position = float3(0.0f);
    hit.radiance = float3(0.0f);
    hit.pdf = 0.0f;

    uint rectCount = count_rect_lights();
    if (rectCount == 0) {
        return hit;
    }

    float nearestT = 1e30f;
    for (uint i = 0; i < hw7Constants.lightCount; ++i) {
        LightData light = lightBuffer[i];
        if (light.type != uint(LightType::Rect) || light.surfaceArea <= 1e-6f) {
            continue;
        }

        float tCandidate;
        float3 candidatePos;
        if (!intersect_rect_light(light, rayOrigin, rayDir, tCandidate, candidatePos)) {
            continue;
        }

        float cosLight = dot(light.dirW, -rayDir);
        if (cosLight <= 1e-6f) {
            continue;
        }

        if (tCandidate < nearestT) {
            float dist2 = tCandidate * tCandidate * dot(rayDir, rayDir);
            float selectionPdf = 1.0f / float(rectCount);
            float solidAnglePdf =
                dist2 / max(light.surfaceArea * cosLight, 1e-6f);

            nearestT = tCandidate;
            hit.hit = true;
            hit.lightIndex = i;
            hit.t = tCandidate;
            hit.position = candidatePos;
            hit.radiance = light.intensity;
            hit.pdf = selectionPdf * solidAnglePdf;
        }
    }

    return hit;
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
    Spectrum pixelAccumulated = Spectrum(0.0);

    for (uint pathIndex = 0; pathIndex < kPathsPerPixel; ++pathIndex) {
        Spectrum accumulated = Spectrum(0.0);
        Spectrum throughput = Spectrum(1.0);
        Spectrum pendingPathThroughput = Spectrum(0.0);
        Spectrum pendingBounceWeight = Spectrum(0.0);
        float pendingBsdfPdf = 0.0f;
        bool hasPendingBsdfSample = false;

        RayDesc currentRay = get_ray_desc(ray);
        currentRay.TMax = 50000.0f;

        for (uint depth = 0; depth < hw7Constants.maxDepth; ++depth) {
            RayPayload payload;
            payload.hit = 0;
            payload.seed = seed;
            payload.materialID = uint(-1);
            payload.etaFlipped = 0;
            payload.hitDistance = 1e30f;

            TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 1, 0, currentRay, payload);
            seed = payload.seed;

            RectLightHit analyticLightHit =
                intersect_rect_lights(currentRay.Origin, currentRay.Direction);
            bool lightBeforeGeometry =
                analyticLightHit.hit &&
                (!payload.hit || analyticLightHit.t + 1e-3f < payload.hitDistance);

            if (lightBeforeGeometry) {
                Spectrum lightEmission = Spectrum(analyticLightHit.radiance);
                if (hasPendingBsdfSample) {
                    float misWeight =
                        power_heuristic(pendingBsdfPdf, analyticLightHit.pdf);
                    accumulated = add(
                        accumulated,
                        mul(
                            mul(pendingPathThroughput, pendingBounceWeight),
                            mul(lightEmission, misWeight)));
                }
                else {
                    accumulated = add(accumulated, mul(throughput, lightEmission));
                }
                hasPendingBsdfSample = false;
                break;
            }

            hasPendingBsdfSample = false;

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

                accumulated = add(accumulated, mul(throughput, envColor));
                break;
            }

            if (payload.materialID == uint(-1)) {
                break;
            }

            VertexInfo vertex = payload.vertex;
            float3 P = vertex.posW;

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

            Spectrum directLighting = Spectrum(0.0f);
            uint directLightSampleCount =
                depth == 0 ? kPrimaryDirectLightSamples : kSecondaryDirectLightSamples;
            for (uint directSampleIndex = 0;
                 directSampleIndex < directLightSampleCount;
                 ++directSampleIndex) {
                DirectLightSample lightSample =
                    sample_rect_light(P, vertex.faceNormalW, seed);
                float cosineFactor = abs(dot(vertex.normalW, lightSample.direction));
                if (lightSample.valid && cosineFactor > 0.0f) {
                    EvalCallableData directEvalData;
                    directEvalData.color = Spectrum(0.0);
                    directEvalData.throughput = Spectrum(1.0);
                    directEvalData.L = lightSample.direction;
                    directEvalData.V = -currentRay.Direction;
                    directEvalData.sampledDir = lightSample.direction;
                    directEvalData.pdf = 0.0f;
                    directEvalData.evalPdf = 0.0f;
                    directEvalData.seed = 1u;
                    directEvalData.eta_flipped = payload.etaFlipped;
                    directEvalData.material_params_index = launchIndex;
                    directEvalData.vertexInfo = vertex;
                    CallShader(fetchData.shader_type_id, directEvalData);

                    float misWeight = power_heuristic(
                        lightSample.pdf,
                        max(directEvalData.evalPdf, 0.0f));
                    Spectrum directContrib = mul(
                        Spectrum(
                            lightSample.radiance *
                            (cosineFactor * misWeight /
                             max(lightSample.pdf, 1e-6f))),
                        directEvalData.color);

                    directLighting = add(directLighting, directContrib);
                }
            }

            if (directLightSampleCount > 1) {
                directLighting = div(directLighting, float(directLightSampleCount));
            }

            accumulated = add(accumulated, mul(throughput, directLighting));

            EvalCallableData sampleEvalData;
            sampleEvalData.color = Spectrum(0.0);
            sampleEvalData.throughput = Spectrum(1.0);
            sampleEvalData.L = vertex.normalW;
            sampleEvalData.V = -currentRay.Direction;
            sampleEvalData.sampledDir = vertex.normalW;
            sampleEvalData.pdf = 0.0f;
            sampleEvalData.evalPdf = 0.0f;
            sampleEvalData.seed = seed;
            sampleEvalData.eta_flipped = payload.etaFlipped;
            sampleEvalData.material_params_index = launchIndex;
            sampleEvalData.vertexInfo = vertex;
            CallShader(fetchData.shader_type_id, sampleEvalData);
            seed = sampleEvalData.seed;

            float pdf = sampleEvalData.pdf;
            if (pdf <= 1e-6f) {
                break;
            }

            if (any(isnan(sampleEvalData.throughput.toFloat3())) ||
                any(isinf(sampleEvalData.throughput.toFloat3())) ||
                any(isnan(sampleEvalData.sampledDir)) ||
                any(isinf(sampleEvalData.sampledDir))) {
                break;
            }

            Spectrum bounceWeight = div(sampleEvalData.throughput, pdf);
            if (any(isnan(bounceWeight.toFloat3())) ||
                any(isinf(bounceWeight.toFloat3()))) {
                break;
            }

            Spectrum throughputBeforeBounce = throughput;
            Spectrum nextThroughput = mul(throughput, bounceWeight);

            if (depth >= hw7Constants.rrStartDepth) {
                float3 thr = nextThroughput.toFloat3();
                float continueProb =
                    min(0.95, max(hw7Constants.rrProbFloor, max(thr.x, max(thr.y, thr.z))));
                if (random_float(seed) > continueProb) {
                    break;
                }
                bounceWeight = div(bounceWeight, continueProb);
                nextThroughput = div(nextThroughput, continueProb);
            }

            pendingPathThroughput = throughputBeforeBounce;
            pendingBounceWeight = bounceWeight;
            pendingBsdfPdf = pdf;
            hasPendingBsdfSample = true;
            throughput = nextThroughput;

            bool isTransmission =
                dot(sampleEvalData.sampledDir, vertex.faceNormalW) < 0.0f;
            float3 offsetNormal =
                isTransmission ? -vertex.faceNormalW : vertex.faceNormalW;

            currentRay.Origin = P + offsetNormal * 1e-3f;
            currentRay.Direction = normalize(sampleEvalData.sampledDir);
            currentRay.TMin = 1e-3f;
            currentRay.TMax = 50000.0f;
        }

        pixelAccumulated = add(pixelAccumulated, accumulated);
    }

    output[pixel] = float4(
        max(div(pixelAccumulated, float(kPathsPerPixel)).toFloat3(), 0.0f), 1.0f);
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
    payload.hitDistance = RayTCurrent();
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
    const uint kMaxShadowTransparencyDepth = 4;
    if (payload.recursionDepth >= kMaxShadowTransparencyDepth) {
        payload.occluded = 1;
        return;
    }

    uint instanceId = InstanceID();
    uint primId = PrimitiveIndex();
    VertexInfo vertex = get_interpolated_vertex(
        instanceId,
        primId,
        float2(attribs.barycentrics.x, attribs.barycentrics.y));

    uint materialId = instanceDescBuffer[instanceId].materialID;
    if (vertex.subsetMaterialID != uint(-1)) {
        materialId = vertex.subsetMaterialID;
    }

    if (materialId == uint(-1)) {
        payload.occluded = 1;
        return;
    }

    MaterialHeader materialHeader = materialHeaderBuffer[materialId];
    FetchCallableData opacityFetchData;
    opacityFetchData.materialBlobID = materialHeader.material_blob_id;
    opacityFetchData.material_params_index = 0;
    opacityFetchData.shader_type_id = 0;
    opacityFetchData.vertexInfo = vertex;

    CallShader(
        hw7Constants.materialFetchCallableBaseIndex +
            hw7Constants.materialOpacityCallableOffset +
            materialHeader.material_type_id,
        opacityFetchData);

    float opacity = asfloat(opacityFetchData.material_params_index);
    if (opacity < 0.25f) {
        float remainingDist = payload.rayTMax - RayTCurrent();
        if (remainingDist > 1e-3f) {
            RayDesc continuationRay;
            continuationRay.Origin =
                WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
            continuationRay.Direction = WorldRayDirection();
            continuationRay.TMin = 1e-3f;
            continuationRay.TMax = remainingDist;

            ShadowPayload continuationPayload;
            continuationPayload.occluded = 0;
            continuationPayload.recursionDepth = payload.recursionDepth + 1;
            continuationPayload.rayTMax = remainingDist;

            TraceRay(
                SceneBVH,
                RAY_FLAG_NONE,
                0xFF,
                1,
                1,
                1,
                continuationRay,
                continuationPayload);

            payload.occluded = continuationPayload.occluded;
            return;
        }
    }

    payload.occluded = 1;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.occluded = 0;
}
