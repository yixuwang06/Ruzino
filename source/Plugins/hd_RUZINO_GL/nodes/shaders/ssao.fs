#version 430 core

uniform vec2 iResolution;
uniform sampler2D colorTex;
uniform sampler2D positionTex;
uniform sampler2D depthTex;

layout(location = 0) out vec4 Color;

const int AO_SAMPLES = 12;

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 vogelDisk(int sampleIndex, int sampleCount, float rotation)
{
    const float goldenAngle = 2.39996323;
    float r = sqrt((float(sampleIndex) + 0.5) / float(sampleCount));
    float theta = float(sampleIndex) * goldenAngle + rotation;
    return r * vec2(cos(theta), sin(theta));
}

vec3 reconstructNormal(vec2 uv, vec3 centerPos, vec2 texelSize)
{
    vec3 rightPos = texture(positionTex, clamp(uv + vec2(texelSize.x, 0.0), vec2(0.0), vec2(1.0))).xyz;
    vec3 upPos = texture(positionTex, clamp(uv + vec2(0.0, texelSize.y), vec2(0.0), vec2(1.0))).xyz;

    vec3 dx = rightPos - centerPos;
    vec3 dy = upPos - centerPos;
    vec3 n = cross(dx, dy);
    if (length(n) < 1E-6) {
        n = cross(dFdx(centerPos), dFdy(centerPos));
    }
    if (length(n) < 1E-6) {
        return vec3(0.0, 0.0, 1.0);
    }
    return normalize(n);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / iResolution;
    vec2 texelSize = 1.0 / iResolution;

    vec3 baseColor = texture(colorTex, uv).rgb;
    vec3 centerPos = texture(positionTex, uv).xyz;
    float centerDepth = texture(depthTex, uv).x;

    if (dot(centerPos, centerPos) < 1E-8) {
        Color = vec4(baseColor, 1.0);
        return;
    }

    vec3 normal = reconstructNormal(uv, centerPos, texelSize);
    float rotation = hash12(gl_FragCoord.xy) * 6.2831853;

    float occlusion = 0.0;
    float totalWeight = 0.0;
    for (int i = 0; i < AO_SAMPLES; ++i) {
        vec2 diskOffset = vogelDisk(i, AO_SAMPLES, rotation);
        vec2 sampleUv = clamp(uv + diskOffset * 10.0 * texelSize, vec2(0.0), vec2(1.0));
        vec3 samplePos = texture(positionTex, sampleUv).xyz;
        float sampleDepth = texture(depthTex, sampleUv).x;

        vec3 diff = samplePos - centerPos;
        float distanceToSample = length(diff);
        if (distanceToSample < 1E-4) {
            continue;
        }

        vec3 sampleDir = diff / distanceToSample;
        float hemisphere = max(dot(normal, sampleDir) - 0.05, 0.0);
        float rangeWeight = 1.0 - smoothstep(0.15, 1.5, distanceToSample);
        float depthWeight = 1.0 - smoothstep(0.002, 0.05, abs(sampleDepth - centerDepth));
        float weight = rangeWeight * depthWeight;

        occlusion += hemisphere * weight;
        totalWeight += weight;
    }

    float ao = 1.0;
    if (totalWeight > 1E-5) {
        ao = 1.0 - 0.85 * occlusion / totalWeight;
    }
    ao = clamp(ao, 0.2, 1.0);

    Color = vec4(baseColor * ao, 1.0);
}
