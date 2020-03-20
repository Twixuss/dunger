#include "..\dep\tl\include\tl\common.h"
#include "..\dep\tl\include\tl\math.h"
#include "..\dep\tl\include\tl\thread.h"
using namespace TL;
#include "shared.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dsound.h>

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "..\dep\Microsoft DirectX SDK\Include\D3DX11.h"
#pragma comment(lib, "d3dx11.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")

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

struct {
	struct {
		bool keys[256];
		struct {
			bool buttons[8];
			v2i position, delta;
			i32 wheel;
		} mouse;
	} current, previous;
	std::string chars;
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
		chars.clear();
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
	std::string const& string() { return chars; }

} input;

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
			case WM_CHAR: {
				input.chars.push_back((char)wp);
				return 0;
			}
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
struct Label {
	v2i p;
	std::string text;
};
struct RenderFrameInfo {
	m3 camMatrix;
	std::vector<DrawCall> drawCalls;
	std::vector<LightCall> lights;
	std::vector<Label> labels;
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
	ID3D11ShaderResourceView* fontTexture;
	ID3D11SamplerState* testSampler;
	ID3D11BlendState* alphaBlend;
	ID3D11BlendState* additiveBlend;
	ID3D11Buffer* tileVertexBuffer;
	ID3D11Buffer* lightVertexBuffer;
	ID3D11ShaderResourceView* tileVertexBufferView;
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

		DHR(D3DX11CreateShaderResourceViewFromFileA(device, DATA "textures/atlas.png", 0, 0, &atlasTexture, 0));
		DHR(D3DX11CreateShaderResourceViewFromFileA(device, DATA "textures/font.png", 0, 0, &fontTexture, 0));

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
	void renderFrame(RenderFrameInfo&& frame) {
#define ATLAS_SIZE	  2
#define TILE_SIZE	  64
#define INV_TILE_SIZE (1.0f / TILE_SIZE)
		tileVertices.clear();
		for (auto dc : frame.drawCalls) {
			for (u32 i = 0; i < 4; ++i) {
				static TileVertex const baseVerts[]{
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
		immediateContext->PSSetShader(tileShader.ps, 0, 0);
		immediateContext->VSSetShaderResources(0, 1, &tileVertexBufferView);
		immediateContext->Draw(tileCount * 6, 0);

		immediateContext->OMSetRenderTargets(1, &resolveTarget.rtv, 0);
		immediateContext->OMSetBlendState(0, v4{}.data(), 0xFFFFFFFF);
		immediateContext->VSSetShader(msaaResolveShader.vs, 0, 0);
		immediateContext->PSSetShader(msaaResolveShader.ps, 0, 0);
		immediateContext->PSSetShaderResources(0, 1, &mainTarget.srv);
		immediateContext->Draw(3, 0);

		lightVertices.clear();
		for (auto l : frame.lights) {
			static LightVertex const baseVerts[]{
				{{}, {-1, -1}},
				{{}, {-1, 1}},
				{{}, {1, -1}},
				{{}, {1, 1}},
			};
			for (u32 i = 0; i < 4; ++i) {
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
		immediateContext->PSSetShader(lightShader.ps, 0, 0);
		immediateContext->VSSetShaderResources(0, 1, &lightVertexBufferView);
		immediateContext->PSSetShaderResources(0, 1, &resolveTarget.srv);
		immediateContext->Draw(lightCount * 6, 0);

		tileVertices.clear();
		f32 const textScale = 16.0f;
		f32 const letterWidth = 3.0f / 4.0f;
		f32 const cwo2 = 2.0f / clientSize.x;
		f32 const cho2 = 2.0f / clientSize.y;
		u32 letterCount = 0;
		for (auto& l : frame.labels) {
			// clang-format off
			m3 textMatrix = {
				textScale * cwo2 * letterWidth,  0,                                 0, 
				0,                               textScale * cho2,                  0, 
				l.p.x * cwo2 - 1,                (clientSize.y - l.p.y) * cho2 - 1, 1,
			};
			// clang-format on

			f32 row = 0.0f;
			f32 column = 0.0f;
			for (unsigned char c : l.text) {
				if (c == '\n') {
					row -= 1.0f;
					column = 0.0f;
					continue;
				}
				float u = (c % 16) * 0.0625f;
				float v = (c / 16) * 0.0625f;
				tileVertices.push_back({V4(1), (textMatrix * v3{column, row - 1.0f, 1}).xy, {u, v + 0.0625f}});
				tileVertices.push_back({V4(1), (textMatrix * v3{column, row, 1}).xy, {u, v}});
				tileVertices.push_back(
					{V4(1), (textMatrix * v3{column + 1.0f, row - 1.0f, 1}).xy, {u + 0.0625f, v + 0.0625f}});
				tileVertices.push_back({V4(1), (textMatrix * v3{column + 1.0f, row, 1}).xy, {u + 0.0625f, v}});
				column += 1.0f;
			}
			letterCount += l.text.size();
		}
		DHR(immediateContext->Map(tileVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy(mapped.pData, tileVertices.data(), tileVertices.size() * sizeof(TileVertex));
		immediateContext->Unmap(tileVertexBuffer, 0);

		immediateContext->OMSetBlendState(alphaBlend, v4{}.data(), 0xFFFFFFFF);
		immediateContext->VSSetShader(tileShader.vs, 0, 0);
		immediateContext->PSSetShader(tileShader.ps, 0, 0);
		immediateContext->VSSetShaderResources(0, 1, &tileVertexBufferView);
		immediateContext->PSSetShaderResources(0, 1, &fontTexture);
		immediateContext->Draw(letterCount * 6, 0);

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
void renderFrame(Renderer* renderer, RenderFrameInfo&& frame) { renderer->renderFrame(std::move(frame)); }

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
	bool volatile canShutdown = false;
} network;

enum class JoinResult {
	ok,
	error,
	badIp,
	noConnection,
};
JoinResult joinServer(std::string const& ip) {

	WSADATA wsaData;
	if (auto err = WSAStartup(MAKEWORD(2, 2), &wsaData); err != 0) {
		printf("WSAStartup failed: %d\n", err);
		return JoinResult::error;
	}

	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(27015);
	addr.sin_addr.S_un.S_addr = inet_addr(ip.data());
	if (addr.sin_addr.S_un.S_addr == INADDR_NONE) {
		return JoinResult::badIp;
	}

	network.socket = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(network.socket, (SOCKADDR*)&addr, sizeof(addr)) != 0) {
		return JoinResult::noConnection;
	}

	CloseHandle(CreateThread(
		0, 0,
		[](void*) -> DWORD {
			char recvbuf[sizeof(Network::ServerMessage) * 1024];
			int iResult;
			for (;;) {
				iResult = recv(network.socket, recvbuf, _countof(recvbuf), 0);
				if (iResult > 0) {
					if (iResult % sizeof(Network::ServerMessage) != 0) {
						puts("bad messages");
						continue;
					}
					View messages((Network::ServerMessage*)recvbuf, iResult / sizeof(Network::ServerMessage));
					for (auto& m : messages) {
						network.queue.push(std::move(m));
					}
				} else if (iResult == 0)
					printf("Connection closed\n");
				else {
					if (network.canShutdown) {
						return 0;
					}
					printf("recv failed: %d\n", WSAGetLastError());
					ASSERT(0);
				}
			}
			return 0;
		},
		0, 0, 0));
	return JoinResult::ok;
}
void leaveServer() {
	network.canShutdown = true;
	if (shutdown(network.socket, SD_SEND) == SOCKET_ERROR) {
		printf("shutdown failed: %d\n", WSAGetLastError());
	}
	closesocket(network.socket);
	WSACleanup();
}

struct Game {
	struct Bullet {
		u32 creator;
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
	struct Sound {
		u32 soundIndex;
		v2 position;
		f32 pitch = map(randomF32(), -1.0f, 1.0f, 0.9f, 1.1f);
		f32 sampleIndex = 0;
	};
	enum class GameState { menu, enteringIp, game };

	Tiles spTiles, mpTiles;
	std::vector<LightCall> lights;
	std::vector<DrawCall> drawCalls;
	std::vector<Bullet> bullets;
	std::vector<Explosion> explosions;
	std::vector<Ember> embers;
	v2 playerPosition;
	v2 playerVelocity{};
	u32 playerHealth = 5;

	struct Enemy {
		v2 position{};
		u32 health = 5;
	};

	GameState gameState = GameState::menu;
	bool multiPlayer = false;
	u32 playerIndex;
	u32 score = 0;
	f32 timeInMid = 0;
	std::unordered_map<u32, Enemy> enemies;

	f32 leaveTimer = 1;

	f32 cameraZoom = 1.0f;
	v2 cameraPos{};

	f32 fireTimer = 0.0f;
	f32 fireDelta = 1.0f;
	i32 shotsAtOnce = 1;

	v2 pixelsInMeters{};
	u32 const tileSize = 64;

	std::vector<std::vector<i16>> soundBuffers;
	std::vector<Sound> sounds;

	Hit raycastBullet(Tiles const& tiles, u32 firingPlayerID, v2 a, v2 b) {
		Hit hit;
		v2 tMin, tMax;
		minmax(a, b, tMin, tMax);
		v2i tMini = max(roundInt(tMin), V2i(0));
		v2i tMaxi = min(roundInt(tMax) + 1, V2i(CHUNK_W));
		for (i32 tx = tMini.x; tx < tMaxi.x; ++tx) {
			for (i32 ty = tMini.y; ty < tMaxi.y; ++ty) {
				ASSERT(tx < CHUNK_W);
				ASSERT(ty < CHUNK_W);
				if (!tiles.get(tx, ty))
					continue;
				v2 normal;
				if (hit = raycastTile(a, b, V2(tx, ty), 0.5f); hit.hit) {
					return hit;
				}
			}
		}
		for (auto& [targetId, target] : enemies) {
			if (targetId == firingPlayerID) {
				continue;
			}
			if (hit = raycastCircle(a, b, target.position, playerRadius); hit.hit) {
				return hit;
			}
		}
		if (playerIndex != firingPlayerID) {
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
		std::vector<i16> testSound;
		u32 soundLength = 24000;
		u32 sampleRate = 48000;
		f32 attack = 500;
		// shot
		testSound.reserve(soundLength);
		for (u32 i = 0; i < soundLength; ++i) {
			f32 t = i / f32(sampleRate);
			f32 pitch = map(i, 0.0f, soundLength, 2.0f, 1.5f);
			f32 volume = map(i, 0.0f, soundLength, 1.0f, 0.0f);
			volume *= volume;
			volume *= min(i / attack, 1.0f);
			f32 sample = 0;
			sample += sign(t * PI * 100.0f * pitch);
			sample += sin(t * PI * 100 * pitch) * 0.5f + frac(t * 100.0f * pitch) * 2;
			sample *= 0.005 * volume;
			 testSound.push_back((i16)(sample * 32767));
			//testSound.push_back(((i16)((i8)(sample * 127))) * 256);
		}
		soundBuffers.push_back(std::move(testSound));
		testSound.clear();
		// explosion
		testSound.reserve(soundLength);
		for (u32 i = 0; i < soundLength; ++i) {
			f32 pitch = map(i, 0.0f, soundLength, 2.0f, 1.5f);
			f32 volume = map(i, 0.0f, soundLength, 1.0f, 0.0f);
			volume *= volume;
			volume *= min(i / attack, 1.0f);
			f32 sample = 0;
			sample += (frac(i * PI / 5510.0f * pitch) * 2 - 1);
			sample += frac(i * PI / 3705.0f) + frac(i * PI / 4170.0f * pitch) * 4;
			sample *= 0.005 * volume;
			 testSound.push_back((i16)(sample * 32767));
			//testSound.push_back(((i16)((i8)(sample * 127))) * 256);
		}
		soundBuffers.push_back(std::move(testSound));
		testSound.clear();
		// hit
		testSound.reserve(soundLength);
		for (u32 i = 0; i < soundLength; ++i) {
			f32 pitchBase = map(i, 0.0f, soundLength, 0.0f, 3.0f);
			f32 pitch = frac(pitchBase) * 2 + map(TL::floor(pitchBase), 0, 2, 1, 2);
			f32 volume = map(i, 0.0f, soundLength, 1.0f, 0.0f);
			volume *= volume;
			volume *= min(i / attack, 1.0f);
			f32 sample = 0;
			sample += (sin(i * PI / 551.0f * pitch) * 2 - 1);
			sample += sin(i * PI / 175.0f) * sin(i * PI / 217.0f * pitch) * 4;
			sample *= 0.01 * volume;
			 testSound.push_back((i16)(sample * 32767));
			//testSound.push_back(((i16)((i8)(sample * 127))) * 256);
		}
		soundBuffers.push_back(std::move(testSound));
		testSound.clear();
		// lvl up
		testSound.reserve(soundLength);
		for (u32 i = 0; i < soundLength; ++i) {
			f32 pitchBase = map(i, 0.0f, soundLength, 0.0f, 3.0f);
			f32 pitch = powf(2, map(TL::floor(pitchBase), 0, 2, 1, 2) + map(TL::floor(pitchBase * 3) / 3, 0, 2, 1, 2));
			f32 volume = map(i, 0.0f, soundLength, 1.0f, 0.5f);
			volume *= volume;
			volume *= min(i / attack, 1.0f);
			f32 sample = 0;
			sample += (sin(i * PI / 551.0f * pitch) * 2 - 1);
			sample += sin(i * PI / 175.0f) * sin(i * PI / 217.0f * pitch) * 4;
			sample *= 0.005 * volume;
			testSound.push_back((i16)(sample * 32767));
			//testSound.push_back(((i16)((i8)(sample * 127))) * 256);
		}
		soundBuffers.push_back(std::move(testSound));
		testSound.clear();
		// get damage
		soundLength = 12000;
		testSound.reserve(soundLength);
		for (u32 i = 0; i < soundLength; ++i) {
			f32 volume = map(i, 0.0f, soundLength, 1.0f, 0.5f);
			volume *= volume;
			volume *= min(i / attack, 1.0f);
			f32 sample = 0;
			sample += (sin(i * PI / 551.0f) * 2 - 1);
			sample += sin(i * PI / 175.0f) * sin(i * PI / 217.0f) * 4;
			sample *= 0.01 * volume * (frac(f32(i) / soundLength * 20) > 0.5f);
			 testSound.push_back((i16)(sample * 32767));
			//testSound.push_back(((i16)((i8)(sample * 127))) * 256);
		}
		soundBuffers.push_back(std::move(testSound));

		bullets.reserve(128);
		explosions.reserve(128);
		embers.reserve(1024);
	}
	~Game() {
		if (multiPlayer) {
			leaveServer();
		}
	}

	u32 samplesPerSecond = 48000;

	void fillSoundBuffer(i16* sample, u32 sampleCount) {
		std::vector<f32> volumes;
		volumes.reserve(sounds.size());
		f32 const volumeFalloff = 10;
		for (auto& s : sounds) {
			f32 dist = distance(s.position, playerPosition);
			volumes.push_back(pow2(volumeFalloff / (dist + volumeFalloff)));
		}
		for (u32 i = 0; i < sampleCount; ++i) {
			i16 newSample = 0;
			auto volumeIt = volumes.begin();
			for (auto& s : sounds) {
				auto& buf = soundBuffers[s.soundIndex];
				u32 sampleIndex = (u32)s.sampleIndex;
				f32 volume = *volumeIt++;
				if (sampleIndex >= buf.size()) {
					continue;
				}
				newSample += buf[sampleIndex] * volume;
				s.sampleIndex += s.pitch;
			}
			*sample++ = newSample;
			*sample++ = newSample;
		}
	}

	void destroyBullet(Bullet& b) { bullets.erase(bullets.begin() + (&b - bullets.data())); };
	void updateBullets(Tiles const& tiles) {
		for (u32 i = 0; i < bullets.size(); ++i) {
			auto& b = bullets[i];
			b.remainingLifeTime -= time.delta;
			if (b.remainingLifeTime <= 0) {
				destroyBullet(b);
				--i;
				continue;
			}
			v2 nextPos = b.position + b.direction * time.delta * 10;
			if (auto hit = raycastBullet(tiles, b.creator, b.position, nextPos); hit.hit) {
				createExplosion(b.position, hit.normal);
				destroyBullet(b);
				--i;
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
	v2 getStartPosition() { return (v2)(v2i{randomI32() & 1, randomI32() & 1} * (CHUNK_W - 3) + 1); }
	bool connectToServer(std::string const& ip, std::string* resultString) {
		if (resultString) {
			switch (joinServer(ip)) {
				case JoinResult::badIp: *resultString = "Bad IP address"; return false;
				case JoinResult::noConnection: *resultString = "Failed to connect to the server"; return false;
				case JoinResult::error: *resultString = "An error occurred"; return false;
				case JoinResult::ok: return true;
			}
		} else {
			switch (joinServer(ip)) {
				case JoinResult::ok: return true;
				default: return false;
			}
		}
		ASSERT(0);
	}
	void update(Renderer* renderer) {
		switch (gameState) {
			case Game::GameState::menu: {
				std::vector<Label> labels;
				labels.push_back({(v2i)clientSize / 4, "Press"});
				labels.push_back({(v2i)clientSize / 4 + v2i{0, 16}, "'Enter' to start single player"});
				labels.push_back({(v2i)clientSize / 4 + v2i{0, 32}, "'Space' to join a server"});

				if (input.keyDown(VK_RETURN)) {
					multiPlayer = false;
					gameState = GameState::game;
					spTiles = generateMap();
					playerPosition = getStartPosition();
				} else if (input.keyDown(' ')) {
					gameState = GameState::enteringIp;
				} else if (input.keyDown('L')) {
					if (connectToServer("127.0.0.1", 0)) {
						multiPlayer = true;
						gameState = GameState::game;
						playerPosition = getStartPosition();
					}
				}

				RenderFrameInfo frame;
				frame.labels = std::move(labels);
				renderFrame(renderer, std::move(frame));
				break;
			}
			case Game::GameState::enteringIp: {
				static std::string inputIp;
				static std::string joinString;

				for (auto c : input.string()) {
					if (c == '\b') {
						if (inputIp.size())
							inputIp.pop_back();
					} else if (c != '\r')
						inputIp.push_back(c);
				}

				std::vector<Label> labels;
				labels.push_back({(v2i)clientSize / 4, "Enter ip address or 'local'"});
				labels.push_back({(v2i)clientSize / 4 + v2i{0, 16}, inputIp});
				labels.push_back({(v2i)clientSize / 4 + v2i{0, 32}, joinString});

				if (input.keyDown(VK_RETURN)) {
					if (inputIp == "local")
						inputIp = "127.0.0.1";
					if (connectToServer(inputIp, &joinString)) {
						multiPlayer = true;
						gameState = GameState::game;
						playerPosition = getStartPosition();
					}
				} else if (input.keyDown(VK_ESCAPE)) {
					gameState = GameState::menu;
				}

				RenderFrameInfo frame;
				frame.labels = std::move(labels);
				renderFrame(renderer, std::move(frame));
				break;
			}
			case Game::GameState::game: updateGame(multiPlayer ? mpTiles : spTiles, renderer); break;
		}
	}
	void createExplosion(v2 position, v2 normal) {
		Explosion ex;
		ex.position = position;
		ex.maxLifeTime = ex.remainingLifeTime = 0.75f + randomF32() * 0.25f;
		ex.rotationOffset = randomF32();
		explosions.push_back(ex);

		Ember e;
		e.position = position;
		for (u32 i = 0; i < 16; ++i) {
			e.maxLifeTime = e.remainingLifeTime = 0.75f + randomF32() * 0.25f;
			e.velocity = normalize(v2{randomF32(), randomF32()} + normal) * (1.0f + randomF32());
			e.rotationOffset = randomF32();
			embers.push_back(e);
		}
		sounds.push_back({1, ex.position});
	}
	void updateGame(Tiles const& tiles, Renderer* renderer) {
		if (multiPlayer) {
			for (;;) {
				if (auto opt = network.pop()) {
					try {
						std::visit(
							Visitor{[&](Network::ChangeEnemyPosition pc) { enemies.at(pc.id).position = pc.position; },
									[&](Network::ChangePosition pc) { playerPosition = pc.position; },
									[&](Network::PlayerConnected pc) {
										printf("PlayerConnected: %u\n", pc.id);
										enemies[pc.id].health = pc.health;
									},
									[&](Network::PlayerDisconnected pc) {
										printf("PlayerDisconnected: %u\n", pc.id);
										enemies.erase(pc.id);
									},
									[&](Network::AssignId ai) {
										printf("AssignId: %u\n", ai.id);
										playerIndex = ai.id;
									},
									[&](Network::CreateBullet b) {
										Bullet nb;
										nb.creator = b.creator;
										nb.id = b.id;
										nb.position = b.position;
										nb.direction = b.direction;
										bullets.push_back(nb);
									},
									[&](Network::EnemyHit h) {
										enemies.at(h.id).health = h.health;
									},
									[&](Network::EnemyKill k) {
										enemies.at(k.id).health = 5;
										++score;
										fireDelta = 1.0f / (score * 0.01f + 1.0f);
										sounds.push_back({2, playerPosition});
									},
									[&](Network::HealthChange h) {
										playerHealth = h.health;
										sounds.push_back({4, playerPosition});
									},
									[&](Network::ExplodeBullet eb) {
										for (auto& b : bullets) {
											if (b.id == eb.id && b.creator == eb.creator) {
												createExplosion(b.position, eb.normal);
												destroyBullet(b);
											}
										}
									},
									[&](Network::GetTiles& gt) { mpTiles = gt.tiles; }},
							*opt);
					} catch (...) {
						puts("std::visit failed");
					}
				} else {
					break;
				}
			}
		}
		for (u32 i = 0; i < sounds.size(); ++i) {
			auto& s = sounds[i];
			if (s.sampleIndex >= soundBuffers[s.soundIndex].size()) {
				sounds.erase(sounds.begin() + i);
				--i;
			}
		}
		cameraZoom -= input.mouseWheel() / 240.0f;
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
					if (!tiles.get(tx, ty))
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

		playerPosition = clamp(newHeroPosition, v2{}, V2(CHUNK_W));
		if (multiPlayer)
			network.push(Network::ChangePosition{playerPosition});

		if (distanceSqr(playerPosition, V2(CHUNK_W / 2)) < midRadius * midRadius) {
			timeInMid += time.delta;
			static auto oldShots = shotsAtOnce;
			shotsAtOnce = u32(timeInMid / 30) + 1;
			if (oldShots != shotsAtOnce)
				sounds.push_back({3, playerPosition});
			oldShots = shotsAtOnce;
		}

		cameraPos = lerp(cameraPos, playerPosition, time.delta * (distance(cameraPos, playerPosition) + 5));
		v2 camOffset = cameraPos - (v2)clientSize * 0.5f / cameraZoom;

		updateExplosions();
		updateEmbers();
		updateBullets(tiles);
		if (fireTimer >= 0)
			fireTimer -= time.delta;
		if (input.mouseHeld(0)) {
			if (fireTimer < 0) {
				fireTimer += fireDelta;
				auto createBullet = [&](f32 offset) {
					static u32 bulletCounter = 0;
					Bullet b;
					v2i mousePos = input.mousePosition();
					mousePos.y = clientSize.y - mousePos.y;
					b.direction = normalize((v2)mousePos - (v2)clientSize / 2 -
											(playerPosition - cameraPos) * pixelsInMeters * 2.0f);
					b.direction = normalize(b.direction + cross(b.direction) * offset * 0.5f);
					b.position = playerPosition;
					b.creator = playerIndex;
					b.id = bulletCounter++;
					bullets.push_back(b);
					if (multiPlayer)
						network.push(Network::CreateBullet{b.creator, b.id, b.position, b.direction});
				};
				if (shotsAtOnce == 1) {
					createBullet(0);
				} else {
					if (shotsAtOnce & 1) {
						for (i32 i = -shotsAtOnce / 2; i <= shotsAtOnce / 2; ++i) {
							createBullet(f32(i) / shotsAtOnce);
						}
					} else {
						for (i32 i = 0; i < shotsAtOnce; ++i) {
							createBullet(f32(i - shotsAtOnce * 0.5f + 0.5f) / shotsAtOnce);
						}
					}
				}
				sounds.push_back({0, playerPosition});
			}
		}
		if (input.keyHeld(VK_ESCAPE)) {
			leaveTimer -= time.delta;
		} else {
			leaveTimer = 1;
		}
		if (leaveTimer <= 0) {
			if (multiPlayer) {
				multiPlayer = false;
				leaveServer();
			}
			gameState = GameState::menu;
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
				submitTile(1, m3::translation(tx, ty), V4(V3(tiles.get(tx, ty) ? 1 : 0.25f), 1.0f));
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
			submitLight(e.position, v3{1, 0.5, 0.1} * t * 4, t * 5);
		}
		for (auto e : embers) {
			f32 t = e.remainingLifeTime / e.maxLifeTime;
			submitTile(3, m3::translation(e.position) * m3::rotationZ((t + e.rotationOffset) * 10),
					   V4(V3(t * 2 + 2), t), 1 - pow2(1 - t));
		}

		auto drawPlayer = [&](v2 position, u32 health) {
			submitTile(0, m3::translation(position));
			submitLight(position, V3(1), 15);
			submitLight(position, V3(normalize(lerp(v2{1, 0}, v2{0, 1}, (health - 1) * 0.2f)) * 2, 0), 3);
		};

		submitLight(V2(CHUNK_W / 2 - 0.5f), v3{0.3, 0.6, 0.9} * 2, midRadius);
		drawPlayer(playerPosition, playerHealth);
		for (auto& [k, e] : enemies) {
			drawPlayer(e.position, e.health);
		}

		std::vector<Label> labels;
		labels.push_back({{16, 16}, std::string("Score: ") + std::to_string(score)});
		labels.push_back({{16, 32}, std::string("Time in mid: ") + std::to_string(timeInMid)});

		RenderFrameInfo frame;
		frame.camMatrix = camMatrix;
		frame.drawCalls = std::move(drawCalls);
		frame.lights = std::move(lights);
		frame.labels = std::move(labels);
		renderFrame(renderer, std::move(frame));

		drawCalls.clear();
		lights.clear();
		if (multiPlayer)
			network.send();
	}
	void resize() { pixelsInMeters = (v2)clientSize / tileSize; }
};

Game* createGame() { return new Game; }
void destroyGame(Game* game) { delete game; }
void updateGame(Game* game, Renderer* renderer) { game->update(renderer); }
void resizeGame(Game* game) { game->resize(); }

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
	u32 msaaSampleCount = 8;

	AllocConsole();
	freopen("CONOUT$", "w", stdout);
	freopen("CONIN$", "r", stdin);

	auto hwnd = createWindow(instance);

	input.init();

	RenderSettings renderSettings;
	renderSettings.msaaSampleCount = 8;
	auto renderer = createRenderer(hwnd, renderSettings);

	Game* game = createGame();

	LPDIRECTSOUND DirectSound;
	DHR(DirectSoundCreate(0, &DirectSound, 0));
	DHR(DirectSound->SetCooperativeLevel(hwnd, DSSCL_PRIORITY));

	DSBUFFERDESC BufferDesc = {};
	BufferDesc.dwSize = sizeof(BufferDesc);
	BufferDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;

	LPDIRECTSOUNDBUFFER PrimaryBuffer;
	DHR(DirectSound->CreateSoundBuffer(&BufferDesc, &PrimaryBuffer, 0));

	WAVEFORMATEX WaveFormat;

	WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
	WaveFormat.nChannels = 2;
	WaveFormat.nSamplesPerSec = 48000;
	WaveFormat.wBitsPerSample = 16;
	u32 bytesPerSample = WaveFormat.wBitsPerSample / 8 * WaveFormat.nChannels;
	WaveFormat.nBlockAlign = bytesPerSample;
	WaveFormat.nAvgBytesPerSec = WaveFormat.nBlockAlign * WaveFormat.nSamplesPerSec;
	WaveFormat.cbSize = sizeof(WaveFormat);

	DHR(PrimaryBuffer->SetFormat(&WaveFormat));

	u32 soundBufferSize = WaveFormat.nSamplesPerSec * bytesPerSample;
	BufferDesc.dwBufferBytes = soundBufferSize;
	BufferDesc.dwFlags = 0;
	BufferDesc.lpwfxFormat = &WaveFormat;

	LPDIRECTSOUNDBUFFER soundBuffer;
	DHR(DirectSound->CreateSoundBuffer(&BufferDesc, &soundBuffer, 0));
	u32 runningSampleIndex = 0;
	auto fillSoundBuffer = [&](DWORD ByteToLock, DWORD BytesToWrite) {
		void* Region1;
		void* Region2;
		DWORD Region1Size;
		DWORD Region2Size;
		DHR(soundBuffer->Lock(ByteToLock, BytesToWrite, &Region1, &Region1Size, &Region2, &Region2Size, 0));
		ASSERT(Region1Size % bytesPerSample == 0);
		ASSERT(Region2Size % bytesPerSample == 0);

		DWORD Region1SampleCount = Region1Size / bytesPerSample;
		DWORD Region2SampleCount = Region2Size / bytesPerSample;

		game->fillSoundBuffer((i16*)Region1, Region1SampleCount);
		game->fillSoundBuffer((i16*)Region2, Region2SampleCount);

		runningSampleIndex += Region1SampleCount + Region2SampleCount;

		DHR(soundBuffer->Unlock(Region1, Region1Size, Region2, Region2Size));
	};
	fillSoundBuffer(0, soundBufferSize);
	DHR(soundBuffer->Play(0, 0, DSBPLAY_LOOPING));

	DWORD LastPlayCursor = 0;

	auto lastPerfCounter = getPerfCounter();
	while (isRunning) {
		input.swap();
		if (lostFocus) {
			lostFocus = false;
			input.reset();
		}
		MSG msg{};
		while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
			bool dispatch = true;
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
					dispatch = false;
					break;
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
			if (dispatch)
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

		auto latencySampleCount = WaveFormat.nSamplesPerSec / 12;
		u32 ByteToLock = (runningSampleIndex * bytesPerSample) % soundBufferSize;
		// DEBUG_HRESULT_CHECK(WinSound.Buffer->GetCurrentPosition(&PlayCursor, &WriteCursor));
		u32 TargetCursor = (LastPlayCursor + latencySampleCount * bytesPerSample) % soundBufferSize;
		// Printf("BTL: %u, TC: %u WC: %u\n", ByteToLock, TargetCursor, WriteCursor);
		u32 BytesToWrite;
		if (ByteToLock > TargetCursor) {
			BytesToWrite = soundBufferSize - ByteToLock + TargetCursor;
		} else {
			BytesToWrite = TargetCursor - ByteToLock;
		}
		if (BytesToWrite) { // TODO: ???
			fillSoundBuffer(ByteToLock, BytesToWrite);
		}

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

		DWORD PlayCursor;
		DWORD WriteCursor;
		DHR(soundBuffer->GetCurrentPosition(&PlayCursor, &WriteCursor));

		LastPlayCursor = PlayCursor;

		char buf[256];
		sprintf(buf, "Dunger! | delta: %.1fms", time.delta * 1000);
		SetWindowTextA(hwnd, buf);
	}
	destroyGame(game);

	return 0;
}
