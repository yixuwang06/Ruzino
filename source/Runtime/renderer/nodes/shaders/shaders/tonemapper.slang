Texture2D<float4> gInputTexture;
RWTexture2D<float4> gOutputTexture;

ConstantBuffer<float> cbToneMappingParams;
ConstantBuffer<uint2> cbImageSize;

float3 aces_fitted(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;

    float3 mapped = (color * (a * color + b)) / (color * (c * color + d) + e);
    return saturate(mapped);
}

[numthreads(16, 16, 1)]
void main(uint2 DTid: SV_DispatchThreadID)
{
    if (DTid.x >= cbImageSize.x || DTid.y >= cbImageSize.y) {
        return;
    }

    float4 inputColor = gInputTexture.Load(int3(DTid, 0));
    float exposure = max(cbToneMappingParams, 0.0f);
    float3 hdr = max(inputColor.rgb * exposure, 0.0f);
    float3 mapped = aces_fitted(hdr);

    gOutputTexture[DTid] = float4(mapped, inputColor.a);
}
