#include "common.hlsl"
struct v2p {
	float2 uv : UV;
};
#if defined COMPILE_VS
void main(in uint id : SV_VertexID, out v2p o, out float4 oPos : SV_Position) {
	const float2 positions[] = {
		float2(-1, -1),
		float2(-1, 3),
		float2(3, -1),
	};
	const float2 uvs[] = {
		float2(0, 1),
		float2(0, -1),
		float2(2, 1),
	};
	o.uv = uvs[id];
	oPos = float4(positions[id], 0, 1);
}
#elif defined COMPILE_PS
cbuffer blur : register(b1) {
	float blurAmount;
	float blurAmountOver2;
}
Texture2D mainTexture : register(t0);
SamplerState testSampler : register(s0);
#ifdef HORIZONTAL
#define CHOOSE (j - blurAmountOver2) * invScreenSize.x, 0
#else 
#define CHOOSE 0, (j - blurAmountOver2) * invScreenSize.y
#endif
float4 main(in v2p i, in float4 iPos : SV_Position) : SV_Target {
	float4 col = 0;
	float div = 0;
	const float w = blurAmount * 0.5f;
	for (int j = 0; j < blurAmount; ++j) {
		const float mul = 1 - abs(j - w) / w;
		//const float mul = 1 - abs(abs(j*4/blurAmount - 2)-1);
		div += mul;
		col += mainTexture.Sample(testSampler, i.uv + float2(CHOOSE)) * mul;
	}
	return col / div;
}
#endif