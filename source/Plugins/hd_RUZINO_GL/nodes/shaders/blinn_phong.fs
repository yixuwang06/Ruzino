#version 430 core

// Define a uniform struct for lights
struct Light {
    // The matrices are used for shadow mapping. You need to fill it according to how we are filling it when building the normal maps (node_render_shadow_mapping.cpp). 
    // Now, they are filled with identity matrix. You need to modify C++ code innode_render_deferred_lighting.cpp.
    // Position and color are filled.
    mat4 light_projection;
    mat4 light_view;
    vec3 position;
    float radius;
    vec3 color; // Just use the same diffuse and specular color.
    int shadow_map_id;
};

layout(binding = 0) buffer lightsBuffer {
  Light lights[];
};

uniform vec2 iResolution;

uniform sampler2D diffuseColorSampler;
uniform sampler2D normalMapSampler; // You should apply normal mapping in rasterize_impl.fs
uniform sampler2D metallicRoughnessSampler;
uniform sampler2DArray shadow_maps;
uniform sampler2D position;

// uniform float alpha;
uniform vec3 camPos;

uniform int light_count;

layout(location = 0) out vec4 Color;

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

    float closestDepth = texture(
        shadow_maps,
        vec3(shadowCoord.xy, lights[lightIndex].shadow_map_id)).x;
    float bias = max(0.0025 * (1.0 - max(dot(worldNormal, lightDir), 0.0)), 0.0005);

    return (shadowCoord.z - bias <= closestDepth) ? 1.0 : 0.0;
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
    float metal = clamp(metalnessRoughness.x, 0.0, 1.0);
    float roughness = clamp(metalnessRoughness.y, 0.0, 1.0);

    float ks = 0.8 * metal;
    float kd = 1.0 - ks;
    float shininess = mix(4.0, 128.0, 1.0 - roughness);

    vec3 viewDir = normalize(camPos - pos);
    vec3 ambient = 0.03 * albedo;
    vec3 result = ambient;

    for (int i = 0; i < light_count; i++) {
        vec3 lightVec = lights[i].position - pos;
        float distanceToLight = length(lightVec);
        if (distanceToLight < 1E-6) {
            continue;
        }

        vec3 lightDir = lightVec / distanceToLight;
        vec3 halfDir = normalize(lightDir + viewDir);

        float attenuation = 1.0 / max(
            distanceToLight * distanceToLight,
            max(lights[i].radius * lights[i].radius, 1E-4));

        float diffuseFactor = max(dot(normal, lightDir), 0.0);
        float specularFactor = pow(max(dot(normal, halfDir), 0.0), shininess);
        float shadowVisibility = ComputeShadowVisibility(i, pos, normal, lightDir);

        vec3 directLight =
            kd * albedo * lights[i].color * diffuseFactor +
            ks * lights[i].color * specularFactor;

        result += shadowVisibility * attenuation * directLight;
    }

    Color = vec4(result, 1.0);
}
