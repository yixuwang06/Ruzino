#version 430 core

struct Light {
    mat4 light_projection;
    mat4 light_view;
    vec3 position;
    float radius;
    vec3 color;
    int shadow_map_id;
};

layout(binding = 0) buffer lightsBuffer {
  Light lights[];
};

uniform vec2 iResolution;
uniform sampler2D diffuseColorSampler;
uniform sampler2D normalMapSampler;
uniform sampler2D metallicRoughnessSampler;
uniform sampler2DArray shadow_maps;
uniform sampler2D position;
uniform vec3 camPos;
uniform int light_count;

layout(location = 0) out vec4 Color;

const vec2 kPoissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432),  vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845),  vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554),  vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507),  vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367),   vec2(0.14383161, -0.14100790)
);

float SampleShadowDepth(int lightIndex, vec2 uv)
{
    return texture(
        shadow_maps,
        vec3(clamp(uv, vec2(0.0), vec2(1.0)), lights[lightIndex].shadow_map_id)).x;
}

float ComputeAverageBlockerDepth(
    int lightIndex,
    vec2 uv,
    float receiverDepth,
    vec2 texelSize,
    float searchRadiusTexels)
{
    float blockerDepthSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 sampleUv = uv + kPoissonDisk[i] * texelSize * searchRadiusTexels;
        float sampledDepth = SampleShadowDepth(lightIndex, sampleUv);
        if (sampledDepth < receiverDepth) {
            blockerDepthSum += sampledDepth;
            blockerCount += 1.0;
        }
    }

    if (blockerCount < 0.5) {
        return -1.0;
    }
    return blockerDepthSum / blockerCount;
}

float ComputePCFFilteredShadow(
    int lightIndex,
    vec2 uv,
    float receiverDepth,
    float bias,
    vec2 texelSize,
    float filterRadiusTexels)
{
    float visibility = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 sampleUv = uv + kPoissonDisk[i] * texelSize * filterRadiusTexels;
        float sampledDepth = SampleShadowDepth(lightIndex, sampleUv);
        visibility += (receiverDepth - bias <= sampledDepth) ? 1.0 : 0.0;
    }
    return visibility / 16.0;
}

float ComputeShadowVisibility(int lightIndex, vec3 worldPos, vec3 worldNormal, vec3 lightDir)
{
    vec4 lightClip =
        lights[lightIndex].light_projection *
        lights[lightIndex].light_view *
        vec4(worldPos, 1.0);

    if (lightClip.w <= 0.0) {
        return 1.0;
    }

    vec3 shadowCoord = lightClip.xyz / lightClip.w;
    shadowCoord = shadowCoord * 0.5 + 0.5;

    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return 1.0;
    }

    float bias = max(0.03 * (1.0 - max(dot(worldNormal, lightDir), 0.0)), 0.006);
    vec2 texelSize = 1.0 / vec2(textureSize(shadow_maps, 0).xy);
    float lightSize = clamp(lights[lightIndex].radius, 0.5, 16.0);
    float searchRadiusTexels = 2.0 + lightSize * 0.75;
    float avgBlockerDepth = ComputeAverageBlockerDepth(
        lightIndex, shadowCoord.xy, shadowCoord.z - bias, texelSize, searchRadiusTexels);

    if (avgBlockerDepth < 0.0) {
        return 1.0;
    }

    float penumbra = clamp(
        (shadowCoord.z - avgBlockerDepth) / max(avgBlockerDepth, 1E-4),
        0.0,
        1.0);
    float filterRadiusTexels = clamp(1.0 + penumbra * lightSize * 24.0, 1.0, 24.0);
    return ComputePCFFilteredShadow(
        lightIndex,
        shadowCoord.xy,
        shadowCoord.z,
        bias,
        texelSize,
        filterRadiusTexels);
}

float QuantizeBand(float x)
{
    if (x < 0.15) return 0.10;
    if (x < 0.40) return 0.32;
    if (x < 0.70) return 0.62;
    return 1.0;
}

void main() {
    vec2 uv = gl_FragCoord.xy / iResolution;

    vec3 pos = texture(position, uv).xyz;
    vec3 normal = normalize(texture(normalMapSampler, uv).xyz);
    vec3 albedo = texture(diffuseColorSampler, uv).xyz;

    if (dot(normal, normal) < 1E-8) {
        Color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 metalnessRoughness = texture(metallicRoughnessSampler, uv);
    float roughness = clamp(metalnessRoughness.y, 0.0, 1.0);
    float shininess = mix(4.0, 96.0, 1.0 - roughness);

    vec3 viewDir = normalize(camPos - pos);
    vec3 result = albedo * 0.06;

    for (int i = 0; i < light_count; i++) {
        vec3 lightVec = lights[i].position - pos;
        float distanceToLight = length(lightVec);
        if (distanceToLight < 1E-6) {
            continue;
        }

        vec3 lightDir = lightVec / distanceToLight;
        vec3 halfDir = normalize(lightDir + viewDir);
        float attenuation = max(lights[i].radius, 1.0) / max(distanceToLight, 1.0);
        float shadowVisibility = ComputeShadowVisibility(i, pos, normal, lightDir);

        float diffuseFactor = max(dot(normal, lightDir), 0.0);
        float specularFactor = pow(max(dot(normal, halfDir), 0.0), shininess);
        float bandedDiffuse = QuantizeBand(diffuseFactor * shadowVisibility);
        float specularBand = specularFactor > 0.55 ? 0.25 : 0.0;

        vec3 toonShade =
            albedo * (0.18 + 0.82 * bandedDiffuse) * lights[i].color +
            specularBand * lights[i].color;
        result += attenuation * toonShade;
    }

    float rim = pow(1.0 - max(dot(normal, viewDir), 0.0), 2.5);
    result += rim * 0.16 * vec3(1.0, 0.96, 0.88);

    float outline = 1.0 - smoothstep(0.12, 0.28, abs(dot(normal, viewDir)));
    result = mix(result, vec3(0.02, 0.02, 0.03), outline * 0.85);

    result = pow(max(result, vec3(0.0)), vec3(1.0 / 2.2));
    Color = vec4(result, 1.0);
}
