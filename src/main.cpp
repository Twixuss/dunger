#include "..\dep\tl\include\tl\common.h"
#include "..\dep\tl\include\tl\math.h"
#include "..\dep\tl\include\tl\thread.h"
using namespace TL;
#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "network.h"

#include "..\dep\Microsoft DirectX SDK\Include\D3DX11.h"
#pragma comment(lib, "../dep/Microsoft DirectX SDK/Lib/x64/d3dx11.lib")
#pragma comment(lib, "Ws2_32.lib")

#define time _time_

#define DHR(call)               \
	do {                        \
		HRESULT dhr = call;     \
		ASSERT(SUCCEEDED(dhr)); \
	} while (0)

#define RELEASE_ZERO(x)   \
	do {                  \
		if (x) {          \
			x->Release(); \
			x = 0;        \
		}                 \
	} while (0)

#define DATA  "../data/"
#define LDATA CONCAT(L, DATA)

bool isRunning = true;
bool wasResized = true;
bool lostFocus = false;
v2u clientSize{800, 600};
bool singlePlayer = 0;

struct {
	struct {
		bool keys[256];
		struct {
			bool buttons[8];
			v2i position, delta;
			i32 wheel;
		} mouse;
	} current, previous;
	void init() {
		current = {};
		previous = {};

		RAWINPUTDEVICE mouse = {};
		mouse.usUsagePage = 0x01;
		mouse.usUsage = 0x02;

		if (!RegisterRawInputDevices(&mouse, 1, sizeof(RAWINPUTDEVICE))) {
			ASSERT(0);
		}
	}
	void swap() {
		previous = current;
		current.mouse.delta = {};
		current.mouse.wheel = {};
	}
	void reset() {
		memset(current.keys, 0, sizeof(current.keys));
		memset(current.mouse.buttons, 0, sizeof(current.mouse.buttons));
	}
	void processKey(u32 key, bool extended, bool alt, bool isRepeated, bool wentUp) {
		if (isRepeated == wentUp) { // Don't handle repeated
			current.keys[key] = !isRepeated;
		}
	}

	bool keyHeld(u8 k) { return current.keys[k]; }
	bool keyDown(u8 k) { return current.keys[k] && !previous.keys[k]; }
	bool keyUp(u8 k) { return !current.keys[k] && previous.keys[k]; }
	bool mouseHeld(u8 k) { return current.mouse.buttons[k]; }
	bool mouseDown(u8 k) { return current.mouse.buttons[k] && !previous.mouse.buttons[k]; }
	bool mouseUp(u8 k) { return !current.mouse.buttons[k] && previous.mouse.buttons[k]; }
	v2i mousePosition() { return current.mouse.position; }
	v2i mouseDelta() { return current.mouse.delta; }
	i32 mouseWheel() { return current.mouse.wheel; }

} input;

i64 getPerfCounter() {
	LARGE_INTEGER r;
	QueryPerformanceCounter(&r);
	return r.QuadPart;
}
i64 const perfFrequency = [] {
	LARGE_INTEGER r;
	QueryPerformanceFrequency(&r);
	return r.QuadPart;
}();
f32 getPerfSeconds(i64 begin, i64 end) { return f32(end - begin) / perfFrequency; }
struct {
	u64 frameCount;
	f32 targetFrameTime;
	f32 delta;
	f32 time;
} time{};

HWND createWindow(HINSTANCE instance) {
	WNDCLASSEXA wc{};
	wc.cbSize = sizeof wc;
	wc.hCursor = LoadCursorA(0, IDC_ARROW);
	wc.hIcon = LoadIconA(0, IDI_SHIELD);
	wc.hInstance = instance;
	wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
		switch (msg) {
			case WM_DESTROY: {
				isRunning = false;
				return 0;
			}
			case WM_SIZE: {
				v2u tempClientSize = {LOWORD(lp), HIWORD(lp)};
				if (tempClientSize.x && tempClientSize.y) {
					clientSize = tempClientSize;
					wasResized = true;
				}
				return 0;
			}
			case WM_KILLFOCUS: {
				lostFocus = true;
				return 0;
			}
		}
		return DefWindowProcA(hwnd, msg, wp, lp);
	};
	wc.lpszClassName = "dunger_window_class";
	RegisterClassExA(&wc);

	u32 windowStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;

	v2i screenSize;
	screenSize.x = GetSystemMetrics(SM_CXSCREEN);
	screenSize.y = GetSystemMetrics(SM_CYSCREEN);

	RECT wr{0, 0, (LONG)clientSize.x, (LONG)clientSize.y};
	AdjustWindowRect(&wr, windowStyle, false);

	v2i windowPos = v2i{wr.left, wr.top} + screenSize / 2 - (v2i)(clientSize / 2);
	v2i windowSize = v2i{wr.right - wr.left, wr.bottom - wr.top};

	return CreateWindowExA(0, wc.lpszClassName, "Dunger!", windowStyle, windowPos.x, windowPos.y, windowSize.x,
						   windowSize.y, 0, 0, instance, 0);
}
struct RenderSettings {
	u32 msaaSampleCount;
};
struct DrawCall {
	v4 col;
	m3 objectMatrix;
	u32 texture;
	f32 scale;
};
struct LightCall {
	v2 position;
	v3 color;
	f32 radius;
};
struct RenderFrameInfo {
	m3 camMatrix;
	std::vector<DrawCall> drawCalls;
	std::vector<LightCall> lights;
};
struct Renderer {
	struct Shader {
		ID3D11VertexShader* vs;
		ID3D11PixelShader* ps;
	};
	struct ShaderBytecode {
		ID3DBlob* code;
		ShaderBytecode() = default;
		ShaderBytecode(ShaderBytecode const& rhs) : code(rhs.code) { code->AddRef(); }
		~ShaderBytecode() {
			if (code)
				code->Release();
		}
	};
	struct RenderTarget {
		ID3D11RenderTargetView* rtv;
		ID3D11ShaderResourceView* srv;
		void release() {
			if (rtv)
				rtv->Release();
			if (srv)
				srv->Release();
		}
	};
	struct TileVertex {
		v4 col;
		v2 pos;
		v2 uv;
	};
	struct LightVertex {
		v4 col;
		v2 pos;
	};
	struct alignas(16) {
		v2 screenSize;
	} screenInfoCBD;

#define MAX_TILE_VERTEX_COUNT  (4 * 1024 * 1024)
#define MAX_LIGHT_VERTEX_COUNT (4 * 1024)

	IDXGISwapChain* swapChain;
	ID3D11Device* device;
	ID3D11DeviceContext* immediateContext;
	ID3D11RenderTargetView* backBuffer = 0;
	ID3D11Buffer* screenInfoCB;
	ID3D11ShaderResourceView* atlasTexture;
	ID3D11SamplerState* testSampler;
	ID3D11BlendState* alphaBlend;
	ID3D11BlendState* additiveBlend;
	ID3D11Buffer* tileVertexBuffer;
	ID3D11ShaderResourceView* tileVertexBufferView;
	ID3D11Buffer* lightVertexBuffer;
	ID3D11ShaderResourceView* lightVertexBufferView;

	Shader tileShader, lightShader, msaaResolveShader;
	RenderTarget mainTarget{}, resolveTarget{};
	RenderSettings currentSettings;
	std::vector<TileVertex> tileVertices;
	std::vector<LightVertex> lightVertices;

	Renderer(HWND hwnd, RenderSettings& settings) {
		currentSettings = settings;

		DXGI_SWAP_CHAIN_DESC swapChainDesc{};
		swapChainDesc.BufferCount = 1;
		swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.BufferDesc.RefreshRate = {60, 1};
		swapChainDesc.BufferDesc.Width = 1;
		swapChainDesc.BufferDesc.Height = 1;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.OutputWindow = hwnd;
		swapChainDesc.SampleDesc = {1, 0};
		swapChainDesc.Windowed = true;

		UINT flags = 0;
#if BUILD_DEBUG
		flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		DHR(D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, flags, 0, 0, D3D11_SDK_VERSION,
										  &swapChainDesc, &swapChain, &device, 0, &immediateContext));
		immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		screenInfoCB = createBuffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE,
									sizeof(screenInfoCBD), 0, 0);
		immediateContext->VSSetConstantBuffers(0, 1, &screenInfoCB);
		immediateContext->PSSetConstantBuffers(0, 1, &screenInfoCB);

		auto makeShader = [&](View<wchar> path) {
			Shader result;
			D3D_SHADER_MACRO defines[2]{};
			defines[0].Name = "COMPILE_VS";
			result.vs = makeVertexShader(compileShader(path, defines, "vs", "main", "vs_5_0"));
			defines[0].Name = "COMPILE_PS";
			result.ps = makePixelShader(compileShader(path, defines, "ps", "main", "ps_5_0"));
			return result;
		};

		tileShader = makeShader(LDATA "shaders/tile.hlsl");
		lightShader = makeShader(LDATA "shaders/light.hlsl");
		msaaResolveShader = makeShader(LDATA "shaders/msaa.hlsl");

		DHR(D3DX11CreateShaderResourceViewFromFileA(device, DATA "tex/atlas.png", 0, 0, &atlasTexture, 0));

		{
			D3D11_SAMPLER_DESC desc{};
			desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			desc.MaxLOD = FLT_MAX;
			DHR(device->CreateSamplerState(&desc, &testSampler));
		}
		immediateContext->PSSetSamplers(0, 1, &testSampler);

		alphaBlend = createBlend(D3D11_BLEND_OP_ADD, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
		additiveBlend = createBlend(D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE);

		tileVertexBuffer = createBuffer(D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE,
										sizeof(TileVertex) * MAX_TILE_VERTEX_COUNT, sizeof(TileVertex),
										D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
		tileVertexBufferView = createStructuredBufferView(tileVertexBuffer, MAX_TILE_VERTEX_COUNT, sizeof(TileVertex));

		lightVertexBuffer = createBuffer(D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE,
										 sizeof(LightVertex) * MAX_LIGHT_VERTEX_COUNT, sizeof(LightVertex),
										 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
		lightVertexBufferView = createStructuredBufferView(lightVertexBuffer, MAX_LIGHT_VERTEX_COUNT,
														   sizeof(LightVertex));
	}
	void resize() {
		if (backBuffer)
			backBuffer->Release();
		DHR(swapChain->ResizeBuffers(1, clientSize.x, clientSize.y, DXGI_FORMAT_UNKNOWN, 0));
		ID3D11Texture2D* backBufferTexture;
		DHR(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBufferTexture)));
		DHR(device->CreateRenderTargetView(backBufferTexture, 0, &backBuffer));
		backBufferTexture->Release();
		setViewport(v2u{}, clientSize);

		createRenderTargets();

		screenInfoCBD.screenSize = (v2)clientSize;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		DHR(immediateContext->Map(screenInfoCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy(mapped.pData, &screenInfoCBD, sizeof(screenInfoCBD));
		immediateContext->Unmap(screenInfoCB, 0);
	}
	void renderFrame(RenderFrameInfo frame) {
#define ATLAS_SIZE	  2
#define TILE_SIZE	  64
#define INV_TILE_SIZE (1.0f / TILE_SIZE)
		tileVertices.clear();
		for (auto dc : frame.drawCalls) {
			for (u32 i = 0; i < 4; ++i) {
				static const TileVertex baseVerts[]{
					{{}, {-.5, -.5}, v2{INV_TILE_SIZE, 1 - INV_TILE_SIZE} / ATLAS_SIZE},
					{{}, {-.5, .5}, v2{INV_TILE_SIZE, INV_TILE_SIZE} / ATLAS_SIZE},
					{{}, {.5, -.5}, v2{1 - INV_TILE_SIZE, 1 - INV_TILE_SIZE} / ATLAS_SIZE},
					{{}, {.5, .5}, v2{1 - INV_TILE_SIZE, INV_TILE_SIZE} / ATLAS_SIZE},
				};
				TileVertex vert = baseVerts[i];
				vert.uv += (v2)v2u{dc.texture % ATLAS_SIZE, dc.texture / ATLAS_SIZE} * (1.0f / ATLAS_SIZE);
				vert.pos *= dc.scale;
				vert.pos = (frame.camMatrix * (dc.objectMatrix * V3(vert.pos, 1))).xy;
				vert.col = dc.col;
				tileVertices.push_back(vert);
			}
		}
		u32 tileCount = frame.drawCalls.size();
		frame.drawCalls.clear();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		DHR(immediateContext->Map(tileVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy(mapped.pData, tileVertices.data(), tileVertices.size() * sizeof(TileVertex));
		immediateContext->Unmap(tileVertexBuffer, 0);

		immediateContext->PSSetShaderResources(0, 1, &atlasTexture);
		immediateContext->ClearRenderTargetView(mainTarget.rtv, v4{.1, .1, .1, 1.}.data());
		immediateContext->OMSetRenderTargets(1, &mainTarget.rtv, 0);
		immediateContext->OMSetBlendState(alphaBlend, v4{}.data(), 0xFFFFFFFF);
		immediateContext->VSSetShader(tileShader.vs, 0, 0);
		immediateContext->VSSetShaderResources(0, 1, &tileVertexBufferView);
		immediateContext->PSSetShader(tileShader.ps, 0, 0);
		immediateContext->Draw(tileCount * 6, 0);

		immediateContext->OMSetRenderTargets(1, &resolveTarget.rtv, 0);
		immediateContext->OMSetBlendState(0, v4{}.data(), 0xFFFFFFFF);
		immediateContext->VSSetShader(msaaResolveShader.vs, 0, 0);
		immediateContext->PSSetShader(msaaResolveShader.ps, 0, 0);
		immediateContext->PSSetShaderResources(0, 1, &mainTarget.srv);
		immediateContext->Draw(3, 0);

		lightVertices.clear();
		for (auto l : frame.lights) {
			for (u32 i = 0; i < 4; ++i) {
				static const LightVertex baseVerts[]{
					{{}, {-.5, -.5}},
					{{}, {-.5, .5}},
					{{}, {.5, -.5}},
					{{}, {.5, .5}},
				};
				LightVertex vert = baseVerts[i];
				// vert.pos *= l.radius;
				vert.pos = (frame.camMatrix * (m3::translation(l.position) * V3(vert.pos * l.radius, 1))).xy;
				vert.col = V4(l.color, 1);
				lightVertices.push_back(vert);
			}
		}
		u32 lightCount = frame.lights.size();
		frame.lights.clear();
		DHR(immediateContext->Map(lightVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy(mapped.pData, lightVertices.data(), lightVertices.size() * sizeof(LightVertex));
		immediateContext->Unmap(lightVertexBuffer, 0);

		immediateContext->ClearRenderTargetView(backBuffer, v4{}.data());
		immediateContext->OMSetBlendState(additiveBlend, v4{}.data(), 0xFFFFFFFF);
		immediateContext->OMSetRenderTargets(1, &backBuffer, 0);
		immediateContext->VSSetShader(lightShader.vs, 0, 0);
		immediateContext->VSSetShaderResources(0, 1, &lightVertexBufferView);
		immediateContext->PSSetShader(lightShader.ps, 0, 0);
		immediateContext->PSSetShaderResources(0, 1, &resolveTarget.srv);
		immediateContext->Draw(lightCount * 6, 0);

		swapChain->Present(1, 0);
	}
	static ShaderBytecode compileShader(View<wchar> path, D3D_SHADER_MACRO const* defines, char const* name,
										char const* entry, char const* target) {
		ShaderBytecode result{};
		ID3DBlob* messages = 0;
		HRESULT hr = D3DCompileFromFile(path.data(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, 0, 0,
										&result.code, &messages);
		if (messages) {
			puts((char const*)messages->GetBufferPointer());
			hr = E_FAIL;
		}
		DHR(hr);
		return result;
	};

#define MAKE_SHADER(Type)                                                                                         \
	ID3D11##Type##Shader* make##Type##Shader(ShaderBytecode code) {                                               \
		ID3D11##Type##Shader* shader = 0;                                                                         \
		DHR(device->Create##Type##Shader(code.code->GetBufferPointer(), code.code->GetBufferSize(), 0, &shader)); \
		ASSERT(shader);                                                                                           \
		return shader;                                                                                            \
	}
	MAKE_SHADER(Vertex)
	MAKE_SHADER(Pixel)
#undef MAKE_SHADER
	ID3D11Buffer* createBuffer(UINT bindFlags, D3D11_USAGE usage, UINT cpuAccess, UINT size, UINT stride, UINT misc,
							   void const* initialData = 0) {
		D3D11_BUFFER_DESC desc{};
		desc.BindFlags = bindFlags;
		desc.Usage = usage;
		desc.CPUAccessFlags = cpuAccess;
		desc.ByteWidth = size;
		desc.StructureByteStride = stride;
		desc.MiscFlags = misc;
		ID3D11Buffer* result;
		if (initialData) {
			D3D11_SUBRESOURCE_DATA d3dInitialData;
			d3dInitialData.pSysMem = initialData;
			DHR(device->CreateBuffer(&desc, &d3dInitialData, &result));
		} else {
			DHR(device->CreateBuffer(&desc, 0, &result));
		}
		return result;
	}
	ID3D11BlendState* createBlend(D3D11_BLEND_OP op, D3D11_BLEND src, D3D11_BLEND dst) {
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = true;
		desc.RenderTarget[0].BlendOp = op;
		desc.RenderTarget[0].SrcBlend = src;
		desc.RenderTarget[0].DestBlend = dst;
		desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		ID3D11BlendState* result;
		DHR(device->CreateBlendState(&desc, &result));
		return result;
	}
	void createRenderTargets() {
		ID3D11Texture2D* mainTex;
		ID3D11Texture2D* resolveTex;
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.ArraySize = 1;
			desc.MipLevels = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.SampleDesc = {currentSettings.msaaSampleCount, 0};
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			desc.Width = clientSize.x;
			desc.Height = clientSize.y;
			DHR(device->CreateTexture2D(&desc, 0, &mainTex));
			desc.SampleDesc = {1, 0};
			DHR(device->CreateTexture2D(&desc, 0, &resolveTex));
		}
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		DHR(device->CreateRenderTargetView(mainTex, &desc, &mainTarget.rtv));
		DHR(device->CreateShaderResourceView(mainTex, 0, &mainTarget.srv));
		mainTex->Release();

		desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		DHR(device->CreateRenderTargetView(resolveTex, &desc, &resolveTarget.rtv));
		DHR(device->CreateShaderResourceView(resolveTex, 0, &resolveTarget.srv));
		resolveTex->Release();
	};
	ID3D11ShaderResourceView* createStructuredBufferView(ID3D11Buffer* buffer, u32 count, u32 stride) {
		ID3D11ShaderResourceView* result;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		desc.Buffer.ElementWidth = stride;
		desc.Buffer.NumElements = count;
		DHR(device->CreateShaderResourceView(buffer, &desc, &result));
		return result;
	};
	void setViewport(f32 x, f32 y, f32 w, f32 h) {
		D3D11_VIEWPORT viewport{};
		viewport.TopLeftX = x;
		viewport.TopLeftY = y;
		viewport.Width = w;
		viewport.Height = h;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		immediateContext->RSSetViewports(1, &viewport);
	}
	void setViewport(v2 topLeft, v2 size) { setViewport(topLeft.x, topLeft.y, size.x, size.y); }
	void setViewport(v2i topLeft, v2i size) { setViewport(topLeft.x, topLeft.y, size.x, size.y); }
	void setViewport(v2u topLeft, v2u size) { setViewport(V2(topLeft), V2(size)); }
	void setViewport(v2 topLeft, v2u size) { setViewport(topLeft, V2(size)); }
};
Renderer* createRenderer(HWND hwnd, RenderSettings& settings) { return new Renderer(hwnd, settings); }
void resizeRenderer(Renderer* renderer) { renderer->resize(); }
void renderFrame(Renderer* renderer, RenderFrameInfo const& frame) { renderer->renderFrame(frame); }

struct {
	void push(Network::ClientMessage msg) { clientQueue.push_back(std::move(msg)); }
	auto pop() { return queue.pop(); }
	void send() {
		int iResult;
		do {
			iResult = ::send(socket, (char*)clientQueue.data(), clientQueue.size() * sizeof(Network::ClientMessage), 0);
		} while (iResult <= 0);
		clientQueue.clear();
	}
	std::vector<Network::ClientMessage> clientQueue;
	SPSC::CircularQueue<Network::ServerMessage, 1024> queue;
	SOCKET socket = INVALID_SOCKET;
} network;

#define CHUNK_W 32
struct Game {
	struct Tile {
		bool exists;
	};
	struct Bullet {
		u32 id;
		f32 remainingLifeTime = 5.0f;
		v2 position;
		v2 direction;
	};
	struct Explosion {
		v2 position;
		f32 remainingLifeTime;
		f32 maxLifeTime;
		f32 rotationOffset;
	};
	struct Ember {
		v2 position;
		v2 velocity;
		f32 remainingLifeTime;
		f32 maxLifeTime;
		f32 rotationOffset;
	};
	struct Hit {
		bool hit;
		v2 point;
		v2 normal;
	};

	Tile tiles[CHUNK_W][CHUNK_W];
	std::vector<LightCall> lights;
	std::vector<DrawCall> drawCalls;
	std::vector<Bullet> bullets;
	std::vector<Explosion> explosions;
	std::vector<Ember> embers;
	v2 playerPosition{1, 1};
	v2 playerVelocity{};
	f32 const playerRadius = 0.25f;

	struct Enemy {
		v2 position{};
		bool updatedThisFrame = false;
	};

	u32 playerIndex;
	std::unordered_map<u32, Enemy> enemies;

	f32 cameraZoom = 1.0f;
	v2 cameraPos{};

	f32 fireTimer = 0.0f;
	f32 fireDelta = 0.1f;

	v2 pixelsInMeters{};
	u32 const tileSize = 64;

	i32 randomSeed = 0;

	auto randomF32() { return (randomSeed = randomize(randomSeed) / 256) * (1.0f / 8388608.f); }
	auto randomBool() { return (randomSeed = randomize(randomSeed)) & 0x10; }
	auto randomI32() { return randomSeed = randomize(randomSeed); }
	Hit raycastLine(v2 a, v2 b, v2 c, v2 d) {
		Hit hit;

		v2 s1 = b - a;
		v2 s2 = d - c;

		v2 st = v2{s1.x, s2.x} * (a.y - c.y) - v2{s1.y, s2.y} * (a.x - c.x);
		st /= s1.x * s2.y - s2.x * s1.y;

		if (st.x >= 0 && st.x <= 1 && st.y >= 0 && st.y <= 1) {
			hit.point = a + (st.y * s1);
			hit.normal = c - d;
			hit.normal = cross(hit.normal);
			if (dot(b - a, hit.normal) > 0)
				hit.normal *= -1;
			hit.hit = true;
		} else {
			hit.hit = false;
		}
		return hit;
	}
	Hit raycastTile(v2 a, v2 b, v2 tile, float size) {
		Hit hit;
		f32 const w = size;
		// clang-format off
			Hit hits[]{
				raycastLine(a, b, tile + v2{-w, w}, tile + v2{ w, w}),
				raycastLine(a, b, tile + v2{ w, w}, tile + v2{ w,-w}),
				raycastLine(a, b, tile + v2{ w,-w}, tile + v2{-w,-w}),
				raycastLine(a, b, tile + v2{-w,-w}, tile + v2{-w, w}),
			};
		// clang-format on
		f32 minDist = FLT_MAX;
		int minIndex = -1;
		for (int i = 0; i < _countof(hits); ++i) {
			if (!hits[i].hit)
				continue;
			f32 len = lengthSqr(a - hits[i].point);
			if (len < minDist) {
				minDist = len;
				minIndex = i;
			}
		}
		if (minIndex == -1) {
			hit.hit = false;
		} else {
			hit = hits[minIndex];
		}
		return hit;
	}

	Hit raycastCircle(v2 a, v2 b, v2 circle, f32 radius) {
		Hit hit;
		int intersections = -1;
		v2 intersection1;
		v2 intersection2;

		v2 d = b - a;

		f32 A = lengthSqr(d);
		f32 B = 2 * dot(d, a - circle);
		f32 C = lengthSqr(a - circle) - radius * radius;

		f32 det = B * B - 4 * A * C;
		if ((A <= 0.0000001) || (det < 0)) {
			// No real solutions.
			intersection1 = {NAN, NAN};
			intersection2 = {NAN, NAN};
			intersections = 0;
		} else if (det == 0) {
			// One solution.
			f32 t = -B / (2 * A);
			intersection1 = a + t * d;
			intersection2 = {NAN, NAN};
			intersections = 1;
		} else {
			// Two solutions.
			f32 s = sqrtf(det);
			intersection1 = a + ((-B + s) / (2 * A)) * d;
			intersection2 = a + ((-B - s) / (2 * A)) * d;
			intersections = 2;
		}

		hit.hit = true;
		if (intersections == 1) {
			hit.point = intersection1; // one intersection
		} else if (intersections == 2) {
			f32 dist1 = distanceSqr(intersection1, a);
			f32 dist2 = distanceSqr(intersection2, a);

			if (dist1 < dist2)
				hit.point = intersection1;
			else
				hit.point = intersection2;
		} else {
			hit.hit = false;
		}
		if (hit.hit) {
			if (distanceSqr(a, hit.point) > distanceSqr(a, b)) {
				hit.hit = false;
			} else {
				hit.normal = normalize(circle - hit.point);
			}
		}

		return hit; // no intersections at all
	}
	Hit raycastBullet(u32 firingPlayerID, v2 a, v2 b) {
		Hit hit{};
		v2 tMin, tMax;
		minmax(a, b, tMin, tMax);
		v2i tMini = max(roundInt(tMin), V2i(0));
		v2i tMaxi = min(roundInt(tMax) + 1, V2i(CHUNK_W));
		for (i32 tx = tMini.x; tx < tMaxi.x; ++tx) {
			for (i32 ty = tMini.y; ty < tMaxi.y; ++ty) {
				ASSERT(tx < CHUNK_W);
				ASSERT(ty < CHUNK_W);
				if (!tiles[tx][ty].exists)
					continue;
				v2 normal;
				if (hit = raycastTile(a, b, V2(tx, ty), 0.5f); hit.hit) {
					return hit;
				}
			}
		}
		for (auto& [k, e] : enemies) {
			if (k == firingPlayerID) {
				continue;
			}
			if (hit = raycastCircle(a, b, e.position, playerRadius); hit.hit) {
				network.push(Network::PlayerHit{k});
				return hit;
			}
		}
		if (firingPlayerID != playerIndex) {
			if (hit = raycastCircle(a, b, playerPosition, playerRadius); hit.hit) {
				return hit;
			}
		}
		return hit;
	}

	void submitLight(v2 position, v3 color, f32 radius) {
		LightCall l;
		l.position = position;
		l.color = color;
		l.radius = radius;
		lights.push_back(l);
	};
	void submitTile(u32 texture, m3 matrix, v4 col = {1, 1, 1, 1}, f32 scale = 1) {
		DrawCall dc;
		dc.objectMatrix = matrix;
		dc.texture = texture;
		dc.scale = scale;
		dc.col = col;
		drawCalls.push_back(dc);
	};

	Game() {
		for (int x = 0; x < CHUNK_W; ++x) {
			tiles[x][0].exists = true;
			tiles[x][CHUNK_W - 1].exists = true;
		}
		for (int y = 0; y < CHUNK_W; ++y) {
			tiles[0][y].exists = true;
			tiles[CHUNK_W - 1][y].exists = true;
		}
		for (int x = 1; x < CHUNK_W - 1; ++x) {
			for (int y = 1; y < CHUNK_W - 1; ++y) {
				tiles[x][y].exists = !(randomI32() & 0x70);
			}
		}
		for (int x = 1; x < 3; ++x) {
			for (int y = 1; y < 3; ++y) {
				tiles[x][y].exists = false;
				tiles[CHUNK_W - 1 - x][y].exists = false;
				tiles[CHUNK_W - 1 - x][CHUNK_W - 1 - y].exists = false;
				tiles[x][CHUNK_W - 1 - y].exists = false;
			}
		}
		bullets.reserve(128);
		explosions.reserve(128);
		embers.reserve(1024);
	}
	void updateBullets() {
		for (u32 i = 0; i < bullets.size(); ++i) {
			auto& b = bullets[i];
			b.remainingLifeTime -= time.delta;
			auto destroyBullet = [&] {
				bullets.erase(bullets.begin() + i);
				--i;
			};
			if (b.remainingLifeTime <= 0) {
				destroyBullet();
				continue;
			}
			v2 nextPos = b.position + b.direction * time.delta * 10;
			if (auto hit = raycastBullet(Network::getPlayerIdFromBulletId(b.id), b.position, nextPos); hit.hit) {
				destroyBullet();
				Explosion ex;
				ex.position = hit.point;
				ex.maxLifeTime = ex.remainingLifeTime = 0.75f + randomF32() * 0.25f;
				ex.rotationOffset = randomF32();
				explosions.push_back(ex);

				Ember e;
				e.position = hit.point;
				for (u32 i = 0; i < 16; ++i) {
					e.maxLifeTime = e.remainingLifeTime = 0.75f + randomF32() * 0.25f;
					e.velocity = normalize(v2{randomF32(), randomF32()} + hit.normal) * (1.0f + randomF32());
					e.rotationOffset = randomF32();
					embers.push_back(e);
				}
				continue;
			}
			b.position = nextPos;
		}
	}
	void updateExplosions() {
		for (u32 i = 0; i < explosions.size(); ++i) {
			auto& e = explosions[i];
			e.remainingLifeTime -= time.delta;
			auto destroyExplosion = [&] {
				explosions.erase(explosions.begin() + i);
				--i;
			};
			if (e.remainingLifeTime <= 0) {
				destroyExplosion();
				continue;
			}
		}
	}
	void updateEmbers() {
		for (u32 i = 0; i < embers.size(); ++i) {
			auto& e = embers[i];
			e.remainingLifeTime -= time.delta;
			auto destroyExplosion = [&] {
				embers.erase(embers.begin() + i);
				--i;
			};
			if (e.remainingLifeTime <= 0) {
				destroyExplosion();
				continue;
			}
			e.position += e.velocity * time.delta;
		}
	}
	void update(Renderer* renderer) {
		for (;;) {
			if (auto opt = network.pop()) {
				try {
					std::visit(
						Visitor{[&](Network::PositionChange pc) { enemies.at(pc.playerIndex).position = pc.position; },
								[&](Network::PlayerConnected pc) {
									printf("PlayerConnected: %u\n", pc.playerIndex);
									enemies[pc.playerIndex];
								},
								[&](Network::PlayerDisconnected pc) {
									printf("PlayerDisconnected: %u\n", pc.playerIndex);
									enemies.erase(pc.playerIndex);
								},
								[&](Network::AssignIndex ai) {
									printf("AssignIndex: %u\n", ai.newIndex);
									playerIndex = ai.newIndex;
								},
								[&](Network::CreateBullet b) {
									Bullet nb;
									nb.position = b.position;
									nb.direction = b.direction;
									nb.id = b.id;
									bullets.push_back(nb);
								},
								[&](Network::PlayerHit ph) {
									v2i newPos;
									do {
										newPos = {abs(randomI32()) % CHUNK_W, abs(randomI32()) % CHUNK_W};
									} while (tiles[newPos.x][newPos.y].exists);
									playerPosition = (v2)newPos;
								},
								[](auto) {}},
						*opt);
				} catch (...) {
					puts("std::visit failed");
				}
			} else {
				break;
			}
		}

		cameraZoom -= input.mouseWheel() * 0.01f;
		cameraZoom = clamp(cameraZoom, 1, 10);

		v2 move{};
		move.x = input.keyHeld('D') - input.keyHeld('A');
		move.y = input.keyHeld('W') - input.keyHeld('S');
		playerVelocity = lerp(playerVelocity, normalize(move, v2{}) * 5, time.delta * 10);
		playerVelocity = moveTowards(playerVelocity, v2{}, time.delta);

		auto newHeroPosition = playerPosition;

		// printf("%.3f %.3f\n", playerVelocity.x, playerVelocity.y);
		for (i32 i = 0; i < 4; ++i) {
			if (lengthSqr(playerVelocity) == 0) {
				break;
			}
			v2 a = newHeroPosition;
			v2 b = a + playerVelocity * time.delta;
			bool hit = false;
			v2i tMin = max(roundInt(min(a, b) - (playerRadius + 0.5f)), V2i(0));
			v2i tMax = min(roundInt(max(a, b) + (playerRadius + 0.5f)) + 1, V2i(CHUNK_W));
			for (i32 tx = tMin.x; tx < tMax.x; ++tx) {
				for (i32 ty = tMin.y; ty < tMax.y; ++ty) {
					if (!tiles[tx][ty].exists)
						continue;
					v2 tilef = V2(tx, ty);

					v2 tileMin = tilef - 0.5f;
					v2 tileMax = tilef + 0.5f;

#if 1
					v2 normal{};
					if (tileMin.x < b.x && b.x < tileMax.x) {
						if (tilef.y < b.y && b.y < tilef.y + 0.5f + playerRadius) {
							newHeroPosition.y = tilef.y + 0.5f + playerRadius;
							normal = {0, 1};
						} else if (tilef.y - 0.5f - playerRadius < b.y && b.y < tilef.y) {
							newHeroPosition.y = tilef.y - 0.5f - playerRadius;
							normal = {0, -1};
						}
					} else if (tileMin.y < b.y && b.y < tileMax.y) {
						if (tilef.x < b.x && b.x < tilef.x + 0.5f + playerRadius) {
							newHeroPosition.x = tilef.x + 0.5f + playerRadius;
							normal = {1, 0};
						} else if (tilef.x - 0.5f - playerRadius < b.x && b.x < tilef.x) {
							newHeroPosition.x = tilef.x - 0.5f - playerRadius;
							normal = {-1, 0};
						}
					} else {
						auto doCorner = [&](v2 corner) {
							if (lengthSqr(b - corner) < playerRadius * playerRadius) {
								normal = normalize(b - corner);
								newHeroPosition = corner + normal * playerRadius;
							}
						};
						if (b.x > tileMax.x && b.y > tileMax.y)
							doCorner(tileMax);
						else if (b.x < tileMin.x && b.y < tileMin.y)
							doCorner(tileMin);
						else if (b.x < tileMin.x && b.y > tileMax.y)
							doCorner(v2{tileMin.x, tileMax.y});
						else if (b.x > tileMax.x && b.y < tileMin.y)
							doCorner(v2{tileMax.x, tileMin.y});
					}
					hit = normal != v2{};
					playerVelocity -= normal * dot(playerVelocity, normal);
					if (hit)
						break;
#else
					v2 playerMin = b - playerRadius;
					v2 playerMax = b + playerRadius;

					v2 uMin = max(tileMin, playerMin);
					v2 uMax = min(tileMax, playerMax);

					if (uMax.x >= uMin.x && uMax.y >= uMin.y) {
						v2 diff = uMax - uMin;
						v2 normal{};
						if (diff.x > diff.y) {
							f32 s = sign(b.y - tilef.y);
							a.y = b.y + diff.y * s;
							normal = {0, s};
						} else {
							f32 s = sign(b.x - tilef.x);
							a.x = b.x + diff.x * s;
							normal = {s, 0};
						}
						playerPosition = a;
						playerVelocity -= normal * dot(playerVelocity, normal);
						hit = true;
						break;
					}
#endif
				}
				if (hit)
					break;
			}
		}
		newHeroPosition += playerVelocity * time.delta;

		playerPosition = newHeroPosition;
		if (!singlePlayer)
			network.push(Network::PositionChange{playerIndex, playerPosition});

		cameraPos = lerp(cameraPos, playerPosition, time.delta * (distance(cameraPos, playerPosition) + 5));
		v2 camOffset = cameraPos - (v2)clientSize * 0.5f / cameraZoom;

		updateExplosions();
		updateEmbers();
		updateBullets();
		if (fireTimer >= 0)
			fireTimer -= time.delta;
		if (input.mouseHeld(0)) {
			if (fireTimer < 0) {
				fireTimer += fireDelta;
				static u32 bulletCounter = 0;
				Bullet b;
				v2i mousePos = input.mousePosition();
				mousePos.y = clientSize.y - mousePos.y;
				b.direction = normalize((v2)mousePos - (v2)clientSize / 2 -
										(playerPosition - cameraPos) * pixelsInMeters * 2.0f);
				b.position = playerPosition;
				b.id = Network::makeBulletID(playerIndex, bulletCounter++);
				bullets.push_back(b);
				network.push(Network::CreateBullet{b.id, b.position, b.direction});
			}
		}

		v2 camScale = V2(1.0f / cameraZoom);
		camScale.x /= aspectRatio(clientSize);
		camScale /= clientSize.y;
		camScale *= tileSize * 2;
		m3 camMatrix = m3::scaling(camScale, 1) * m3::translation(-cameraPos);

		for (i32 tx = 0; tx < CHUNK_W; ++tx) {
			for (i32 ty = 0; ty < CHUNK_W; ++ty) {
				// if (!tiles[tx][ty].exists)
				//	continue;
				submitTile(1, m3::translation(tx, ty), V4(V3(tiles[tx][ty].exists ? 1 : 0.25f), 1.0f));
			}
		}
		for (auto b : bullets) {
			submitTile(2, m3::translation(b.position));
			submitLight(b.position, v3{1, 0.2, 0.1}, 2);
		}
		for (auto e : explosions) {
			f32 t = e.remainingLifeTime / e.maxLifeTime;
			submitTile(3, m3::translation(e.position) * m3::rotationZ((t + e.rotationOffset) * 10),
					   V4(V3(t * 2 + 2), t), 1 - pow2(1 - t));
			submitLight(e.position, v3{1, 0.5, 0.1} * t * 2, t * 10);
		}
		for (auto e : embers) {
			f32 t = e.remainingLifeTime / e.maxLifeTime;
			submitTile(3, m3::translation(e.position) * m3::rotationZ((t + e.rotationOffset) * 10),
					   V4(V3(t * 2 + 2), t), 1 - pow2(1 - t));
		}
		submitTile(0, m3::translation(playerPosition));
		submitLight(playerPosition, {1, 1, 1}, 30);

		for (auto& [k, e] : enemies) {
			submitTile(0, m3::translation(e.position));
			submitLight(e.position, {1, .1, .1}, 30);
		}

		renderFrame(renderer, {camMatrix, drawCalls, lights});
		drawCalls.clear();
		lights.clear();
		network.send();
	}
	void resize() { pixelsInMeters = (v2)clientSize / tileSize; }
};

Game* createGame() { return new Game; }
void updateGame(Game* game, Renderer* renderer) { game->update(renderer); }
void resizeGame(Game* game) { game->resize(); }

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
	u32 msaaSampleCount = 8;

	AllocConsole();
	freopen("CONOUT$", "w", stdout);
	freopen("CONIN$", "r", stdin);

	DEFER {
		if (!singlePlayer) {
			WSACleanup();
		}
	};
	if (!singlePlayer) {

		WSADATA wsaData;
		if (auto err = WSAStartup(MAKEWORD(2, 2), &wsaData); err != 0) {
			printf("WSAStartup failed: %d\n", err);
			return 1;
		}

		SOCKADDR_IN addr;
		addr.sin_family = AF_INET;
		addr.sin_port = htons(27015);
		do {
			puts("Enter server ip address:");
			std::string ip;
			std::cin >> ip;

			addr.sin_addr.S_un.S_addr = inet_addr(ip.data());
		} while (addr.sin_addr.S_un.S_addr == INADDR_NONE);

		network.socket = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(network.socket, (SOCKADDR*)&addr, sizeof(addr)) != 0) {
			puts("Failed to connect to server");
			system("pause");
			return -1;
		}

#define DEFAULT_BUFLEN 512

		int iResult;

		CloseHandle(CreateThread(
			0, 0,
			[](void*) -> DWORD {
				char recvbuf[DEFAULT_BUFLEN];
				int iResult;
				for (;;) {
					iResult = recv(network.socket, recvbuf, _countof(recvbuf), 0);
					if (iResult > 0) {
						network.queue.push(*(Network::ServerMessage*)recvbuf);
					} else if (iResult == 0)
						printf("Connection closed\n");
					else
						printf("recv failed: %d\n", WSAGetLastError());
				}
				while (iResult > 0)
					;
				return 0;
			},
			0, 0, 0));
	}
	DEFER {
		if (!singlePlayer) {
			int iResult = shutdown(network.socket, SD_SEND);
			if (iResult == SOCKET_ERROR) {
				printf("shutdown failed: %d\n", WSAGetLastError());
				closesocket(network.socket);
				ASSERT(0);
			}
			closesocket(network.socket);
		}
	};
	auto hwnd = createWindow(instance);

	input.init();

	RenderSettings renderSettings;
	renderSettings.msaaSampleCount = 8;
	auto renderer = createRenderer(hwnd, renderSettings);

	Game* game = createGame();

	auto lastPerfCounter = getPerfCounter();
	while (isRunning) {
		input.swap();
		if (lostFocus) {
			lostFocus = false;
			input.reset();
		}
		MSG msg{};
		while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
			switch (msg.message) {
				case WM_KEYUP:
				case WM_KEYDOWN:
				case WM_SYSKEYUP:
				case WM_SYSKEYDOWN: {
					auto code = (u32)msg.wParam;
					bool extended = msg.lParam & u32(1 << 24);
					bool alt = msg.lParam & u32(1 << 29);
					bool isRepeated = msg.lParam & u32(1 << 30);
					bool wentUp = msg.lParam & u32(1 << 31);
					if (code == VK_F4 && alt) {
						DestroyWindow(hwnd);
					}
					input.processKey(code, extended, alt, isRepeated, wentUp);
					continue;
				}
				case WM_INPUT: {
					RAWINPUT rawInput;
					if (UINT rawInputSize = sizeof(rawInput);
						GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, &rawInput, &rawInputSize,
										sizeof(RAWINPUTHEADER)) == -1) {
						INVALID_CODE_PATH("Error: GetRawInputData");
					}
					if (rawInput.header.dwType == RIM_TYPEMOUSE) {
						auto& mouse = rawInput.data.mouse;
						input.current.mouse.delta += {mouse.lLastX, mouse.lLastY};
						if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
							input.current.mouse.wheel += (i16)mouse.usButtonData;

						RECT clientRect;
						GetClientRect(hwnd, &clientRect);

						// MSDN says that bottom-right corner coordinates are exclusive, but bottom value seems to be
						// inclusive
						bool isInClient = input.current.mouse.position.x >= clientRect.left &&
										  input.current.mouse.position.x < clientRect.right &&
										  input.current.mouse.position.y >= clientRect.top &&
										  input.current.mouse.position.y <= clientRect.bottom;
						if (isInClient) {
							if (mouse.usButtonFlags & RI_MOUSE_BUTTON_1_DOWN)
								input.current.mouse.buttons[0] = 1;
							if (mouse.usButtonFlags & RI_MOUSE_BUTTON_2_DOWN)
								input.current.mouse.buttons[1] = 1;
							if (mouse.usButtonFlags & RI_MOUSE_BUTTON_3_DOWN)
								input.current.mouse.buttons[2] = 1;
							if (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
								input.current.mouse.buttons[3] = 1;
							if (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
								input.current.mouse.buttons[4] = 1;
						}
						if (mouse.usButtonFlags & RI_MOUSE_BUTTON_1_UP)
							input.current.mouse.buttons[0] = 0;
						if (mouse.usButtonFlags & RI_MOUSE_BUTTON_2_UP)
							input.current.mouse.buttons[1] = 0;
						if (mouse.usButtonFlags & RI_MOUSE_BUTTON_3_UP)
							input.current.mouse.buttons[2] = 0;
						if (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP)
							input.current.mouse.buttons[3] = 0;
						if (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP)
							input.current.mouse.buttons[4] = 0;
					}
					continue;
				}
			}
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		{
			POINT p;
			GetCursorPos(&p);
			ScreenToClient(hwnd, &p);
			input.current.mouse.position.x = p.x;
			input.current.mouse.position.y = p.y;
		}
		if (wasResized) {
			wasResized = false;
			resizeGame(game);
			resizeRenderer(renderer);
		}
		updateGame(game, renderer);
		// if (!fullscreen) {
		auto secondsElapsed = getPerfSeconds(lastPerfCounter, getPerfCounter());
		if (secondsElapsed < time.targetFrameTime) {
			i32 msToSleep = (i32)((time.targetFrameTime - secondsElapsed) * 1000.0f);
			if (msToSleep > 0) {
				Sleep((DWORD)msToSleep);
			}
			auto targetCounter = lastPerfCounter + i64(time.targetFrameTime * perfFrequency);
			while (getPerfCounter() < targetCounter)
				;
		}
		//}
		auto endCounter = getPerfCounter();
		time.delta = getPerfSeconds(lastPerfCounter, endCounter);
		lastPerfCounter = endCounter;
		time.time += time.delta;
		++time.frameCount;

		char buf[256];
		sprintf(buf, "Dunger! | delta: %.1fms", time.delta * 1000);
		SetWindowTextA(hwnd, buf);
	}

	return 0;
}
