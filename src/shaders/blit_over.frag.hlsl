// The VR menu (the SDK overlay drawn over transparent black, so premultiplied) over the window image: sampled as
// is, blended ONE / INV_SRC_ALPHA.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

float4 PSMain(VSOutput input) : SV_TARGET {
    return sourceTexture.Sample(sourceSampler, input.uv);
}
