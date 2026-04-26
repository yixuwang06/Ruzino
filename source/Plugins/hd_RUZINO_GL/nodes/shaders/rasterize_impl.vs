#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(std430, binding = 0) buffer buffer0 {
vec2 data[];
}
aTexcoord;

out vec3 vertexPosition;
out vec3 vertexNormal;
out vec2 vTexcoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform sampler2D normalMapSampler;

void main() {
vTexcoord = aTexcoord.data[gl_VertexID];
vTexcoord.y = 1.0 - vTexcoord.y;

vec3 sourceMap = textureLod(normalMapSampler, vTexcoord, 0.0).xyz;
float grayscaleSpread = max(
    abs(sourceMap.r - sourceMap.g),
    max(abs(sourceMap.g - sourceMap.b), abs(sourceMap.r - sourceMap.b)));
bool useDisplacement = grayscaleSpread < 0.05;
float displacement = useDisplacement ? (sourceMap.r - 0.5) * 0.12 : 0.0;

vec3 displacedPos = aPos + aNormal * displacement;

gl_Position = projection * view * model * vec4(displacedPos, 1.0);
vec4 vPosition = model * vec4(displacedPos, 1.0);
vertexPosition = vPosition.xyz / vPosition.w;
vertexNormal = (inverse(transpose(mat3(model))) * aNormal);
}
