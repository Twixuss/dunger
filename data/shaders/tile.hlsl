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
	const uint map[6] = { 0,1,2,2,1,3 };
	Vertex v = vertices[map[id%6]+id/6*4];
	o.uv = v.uv;
	o.color = v.color;
	o.position = float4(v.position, 0, 1);
}
#elif defined COMPILE_PS
Texture2D tileAtlas : register(t0);
SamplerState testSampler : register(s0);
float4 main(in v2p i) : SV_Target {
	float4 col = tileAtlas.Sample(testSampler, i.uv) * i.color;
	return col;
}
#endif