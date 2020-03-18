#include "common.hlsl"
struct v2p {
	float4 position : SV_Position;
	float4 color : COLOR;
	float2 uv : UV;
};
#if defined COMPILE_VS
struct Vertex {
	float4 color;
	float2 position;
	float2 uv;
};
StructuredBuffer<Vertex> vertices : register(t0);
void main(in uint id : SV_VertexID, out v2p o){
	const float2 uvs[] = {
		float2(-1, 1),
		float2(-1,-1),
		float2( 1, 1),
		float2( 1,-1),
	};
	const uint map[6] = { 0,1,2,2,1,3 };
	uint vertexIndex = map[id%6];
	uint tileIndex = id/6*4;
	Vertex v = vertices[vertexIndex+tileIndex];
	o.uv = uvs[vertexIndex];
	o.color = v.color;
	o.position = float4(v.position, 0, 1);
}
#elif defined COMPILE_PS
Texture2D mainTexture : register(t0);
SamplerState testSampler : register(s0);
float lenSqr(float2 a) { return dot(a,a); }
float pow2(float v) { return v*v; }
float invpow2(float v) { return 1-pow2(1-v); }
float4 main(in v2p i) : SV_Target {
	float3 col = mainTexture.Sample(testSampler, i.position.xy / screenSize).xyz;

	float3 result = 0;
	float ld = max(0,1-lenSqr(i.uv));
	result += col * pow2(ld * ld) * i.color.xyz;
	return float4(result,1);
}
#endif