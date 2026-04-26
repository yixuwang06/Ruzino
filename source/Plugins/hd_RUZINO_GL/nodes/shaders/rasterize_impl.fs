#version 430

layout(location = 0) out vec3 position;
layout(location = 1) out float depth;
layout(location = 2) out vec2 texcoords;
layout(location = 3) out vec3 diffuseColor;
layout(location = 4) out vec2 metallicRoughness;
layout(location = 5) out vec3 normal;

in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vTexcoord;
uniform mat4 projection;
uniform mat4 view;

uniform sampler2D diffuseColorSampler;

// This only works for current scenes provided by the TAs 
// because the scenes we provide is transformed from gltf
uniform sampler2D normalMapSampler;
uniform sampler2D metallicRoughnessSampler;

void main() {
    position = vertexPosition;
    vec4 clipPos = projection * view * (vec4(position, 1.0));
    depth = clipPos.z / clipPos.w;
    texcoords = vTexcoord;

    diffuseColor = texture2D(diffuseColorSampler, vTexcoord).xyz;
    metallicRoughness = texture2D(metallicRoughnessSampler, vTexcoord).zy;

    vec3 dpdx = dFdx(vertexPosition);
    vec3 dpdy = dFdy(vertexPosition);
    vec2 duvdx = dFdx(vTexcoord);
    vec2 duvdy = dFdy(vTexcoord);
    vec3 shadingNormalHint = normalize(vertexNormal);
    vec3 geometricNormal = normalize(cross(dpdx, dpdy));
    if (dot(geometricNormal, shadingNormalHint) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    vec3 sampledMap = texture2D(normalMapSampler, vTexcoord).xyz;
    vec3 normalmap_value = sampledMap * 2.0 - 1.0;
    float grayscaleSpread = max(
        abs(sampledMap.r - sampledMap.g),
        max(abs(sampledMap.g - sampledMap.b), abs(sampledMap.r - sampledMap.b)));
    bool useDisplacementNormal = grayscaleSpread < 0.05;

    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    vec3 tangent;
    vec3 bitangent;

    if (abs(det) > 1E-7) {
        tangent = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
        bitangent = (-dpdx * duvdy.x + dpdy * duvdx.x) / det;
    } else {
        tangent = normalize(cross(vec3(0.0, 1.0, 0.0), geometricNormal));
        if (length(tangent) < 1E-7) {
            tangent = normalize(cross(vec3(1.0, 0.0, 0.0), geometricNormal));
        }
        bitangent = normalize(cross(geometricNormal, tangent));
    }

    tangent = normalize(tangent - dot(tangent, geometricNormal) * geometricNormal);
    bitangent = normalize(bitangent - dot(bitangent, geometricNormal) * geometricNormal -
                          dot(bitangent, tangent) * tangent);

    if (useDisplacementNormal ||
        (length(normalmap_value.xy) < 1E-5 && normalmap_value.z > 0.9999)) {
        // Flat/default normal map should preserve the interpolated geometry normal.
        normal = geometricNormal;
    } else {
        mat3 tbn = mat3(tangent, bitangent, geometricNormal);
        normal = normalize(tbn * normalize(normalmap_value));
    }
}
