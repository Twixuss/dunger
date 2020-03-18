#define MSAA_SAMPLE_COUNT 8
#if defined COMPILE_VS
void main(in uint id : SV_VertexID, out float4 oPos : SV_Position){
	const float2 positions[] = {
		float2(-1,-1),
		float2(-1, 3),
		float2( 3,-1),
	};
	oPos = float4(positions[id], 0, 1);
}
#elif defined COMPILE_PS
Texture2DMS<float4> mainTexture : register(t0);
SamplerState testSampler : register(s0);
float4 main(in float4 iPos : SV_Position) : SV_Target {
	float4 col = 0;
	for(int i=0;i<8;++i)
		col += mainTexture.Load(iPos.xy, i);
	return col * 0.125f;
}
#endif