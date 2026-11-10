/**
 * @file blackhole_overlay_d3d11.c
 * @brief Direct3D 11 black-hole renderer for captured desktop frames.
 */

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include "blackhole_overlay_internal.h"
#include "blackhole_overlay_shader.h"

#include <string.h>

typedef struct {
    IDXGISwapChain* swapChain;
    ID3D11RenderTargetView* target;
    float seed[4];
} BlackHoleSurface;

typedef struct {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGIFactory* factory;
    ID3D11VertexShader* vertexShader;
    ID3D11PixelShader* pixelShader;
    ID3D11ShaderResourceView* snapshotView;
    ID3D11SamplerState* sampler;
    ID3D11Buffer* constants;
    unsigned int seed;
} BlackHoleRenderer;

typedef struct {
    float scene[4];
    float monitor[4];
    float seed[4];
} BlackHoleConstants;

/* Integer mixing preserves entropy from long uptimes and negative coordinates. */
static unsigned int MixSeed(unsigned int value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

static void ReleaseSurface(BlackHoleMonitor* monitor) {
    BlackHoleSurface* surface = monitor ? (BlackHoleSurface*)monitor->surface : NULL;
    if (!surface) return;
    if (surface->target) ID3D11RenderTargetView_Release(surface->target);
    if (surface->swapChain) IDXGISwapChain_Release(surface->swapChain);
    free(surface);
    monitor->surface = NULL;
}

static void ReleaseRenderer(BlackHoleRenderer* renderer) {
    if (!renderer) return;
    if (renderer->constants) ID3D11Buffer_Release(renderer->constants);
    if (renderer->sampler) ID3D11SamplerState_Release(renderer->sampler);
    if (renderer->snapshotView) ID3D11ShaderResourceView_Release(renderer->snapshotView);
    if (renderer->pixelShader) ID3D11PixelShader_Release(renderer->pixelShader);
    if (renderer->vertexShader) ID3D11VertexShader_Release(renderer->vertexShader);
    if (renderer->factory) IDXGIFactory_Release(renderer->factory);
    if (renderer->context) ID3D11DeviceContext_Release(renderer->context);
    if (renderer->device) ID3D11Device_Release(renderer->device);
    free(renderer);
}

static BOOL CompileShader(const char* entry, const char* target, ID3DBlob** blob) {
    ID3DBlob* errors = NULL;
    char shader[sizeof(kBlackHoleShaderPart1) + sizeof(kBlackHoleShaderPart2) +
                sizeof(kBlackHoleShaderPart3) - 2];
    memcpy(shader, kBlackHoleShaderPart1, sizeof(kBlackHoleShaderPart1) - 1);
    memcpy(shader + sizeof(kBlackHoleShaderPart1) - 1,
        kBlackHoleShaderPart2, sizeof(kBlackHoleShaderPart2) - 1);
    memcpy(shader + sizeof(kBlackHoleShaderPart1) + sizeof(kBlackHoleShaderPart2) - 2,
        kBlackHoleShaderPart3, sizeof(kBlackHoleShaderPart3));
    HRESULT result = D3DCompile(shader, sizeof(shader) - 1,
        NULL, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob, &errors);
    if (errors) ID3D10Blob_Release(errors);
    return SUCCEEDED(result) && *blob;
}

static BOOL CreateShaders(BlackHoleRenderer* renderer) {
    ID3DBlob* vertex = NULL;
    ID3DBlob* pixel = NULL;
    BOOL success = CompileShader("VSMain", "vs_4_0", &vertex) &&
        CompileShader("PSMain", "ps_4_0", &pixel) &&
        SUCCEEDED(ID3D11Device_CreateVertexShader(renderer->device,
            ID3D10Blob_GetBufferPointer(vertex), ID3D10Blob_GetBufferSize(vertex),
            NULL, &renderer->vertexShader)) &&
        SUCCEEDED(ID3D11Device_CreatePixelShader(renderer->device,
            ID3D10Blob_GetBufferPointer(pixel), ID3D10Blob_GetBufferSize(pixel),
            NULL, &renderer->pixelShader));
    if (vertex) ID3D10Blob_Release(vertex);
    if (pixel) ID3D10Blob_Release(pixel);
    return success;
}

static BOOL CreateSnapshotTexture(BlackHoleSession* session,
                                  BlackHoleRenderer* renderer) {
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = (UINT)session->desktopWidth;
    desc.Height = (UINT)session->desktopHeight;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { session->snapshotPixels,
        (UINT)session->desktopWidth * 4, 0 };
    ID3D11Texture2D* texture = NULL;
    HRESULT result = ID3D11Device_CreateTexture2D(renderer->device, &desc,
        &data, &texture);
    if (SUCCEEDED(result)) result = ID3D11Device_CreateShaderResourceView(
        renderer->device, (ID3D11Resource*)texture, NULL, &renderer->snapshotView);
    if (texture) ID3D11Texture2D_Release(texture);
    return SUCCEEDED(result);
}

BOOL BlackHoleOverlay_CreateRenderer(BlackHoleSession* session) {
    if (!session || !session->snapshotPixels) return FALSE;
    BlackHoleRenderer* renderer = calloc(1, sizeof(*renderer));
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL selected;
    HRESULT result = renderer ? D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE,
        NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, _countof(levels),
        D3D11_SDK_VERSION, &renderer->device, &selected, &renderer->context) : E_OUTOFMEMORY;
    if (FAILED(result)) result = renderer ? D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP,
        NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, _countof(levels),
        D3D11_SDK_VERSION, &renderer->device, &selected, &renderer->context) : E_OUTOFMEMORY;
    IDXGIDevice* dxgiDevice = NULL;
    IDXGIAdapter* adapter = NULL;
    if (FAILED(result) || FAILED(ID3D11Device_QueryInterface(renderer->device,
        &IID_IDXGIDevice, (void**)&dxgiDevice)) ||
        FAILED(IDXGIDevice_GetAdapter(dxgiDevice, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory,
            (void**)&renderer->factory)) || !CreateShaders(renderer) ||
        !CreateSnapshotTexture(session, renderer)) {
        if (adapter) IDXGIAdapter_Release(adapter);
        if (dxgiDevice) IDXGIDevice_Release(dxgiDevice);
        ReleaseRenderer(renderer);
        return FALSE;
    }
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgiDevice);
    D3D11_SAMPLER_DESC sampler = {0};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC constants = { sizeof(BlackHoleConstants), D3D11_USAGE_DEFAULT,
        D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (FAILED(ID3D11Device_CreateSamplerState(renderer->device, &sampler,
        &renderer->sampler)) || FAILED(ID3D11Device_CreateBuffer(renderer->device,
        &constants, NULL, &renderer->constants))) {
        ReleaseRenderer(renderer);
        return FALSE;
    }
    LARGE_INTEGER counter = {0};
    QueryPerformanceCounter(&counter);
    ULONGLONG ticks = GetTickCount64();
    renderer->seed = MixSeed((unsigned int)counter.QuadPart ^
        (unsigned int)((ULONGLONG)counter.QuadPart >> 32) ^
        (unsigned int)ticks ^ (unsigned int)(ticks >> 32));
    session->renderer = renderer;
    return TRUE;
}

BOOL BlackHoleOverlay_CreateMonitorSurface(BlackHoleMonitor* monitor) {
    BlackHoleRenderer* renderer = monitor && monitor->session
        ? (BlackHoleRenderer*)monitor->session->renderer : NULL;
    if (!renderer || !monitor->hwnd) return FALSE;
    BlackHoleSurface* surface = calloc(1, sizeof(*surface));
    DXGI_SWAP_CHAIN_DESC desc = {0};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = monitor->rect.right - monitor->rect.left;
    desc.BufferDesc.Height = monitor->rect.bottom - monitor->rect.top;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = monitor->hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Texture2D* backBuffer = NULL;
    HRESULT result = surface ? IDXGIFactory_CreateSwapChain(renderer->factory,
        (IUnknown*)renderer->device, &desc, &surface->swapChain) : E_OUTOFMEMORY;
    if (SUCCEEDED(result)) result = IDXGISwapChain_GetBuffer(surface->swapChain, 0,
        &IID_ID3D11Texture2D, (void**)&backBuffer);
    if (SUCCEEDED(result)) result = ID3D11Device_CreateRenderTargetView(renderer->device,
        (ID3D11Resource*)backBuffer, NULL, &surface->target);
    if (backBuffer) ID3D11Texture2D_Release(backBuffer);
    if (FAILED(result)) {
        if (surface && surface->target) ID3D11RenderTargetView_Release(surface->target);
        if (surface && surface->swapChain) IDXGISwapChain_Release(surface->swapChain);
        free(surface);
        return FALSE;
    }
    unsigned int seed = MixSeed(renderer->seed ^ MixSeed((unsigned int)monitor->rect.left) ^
                                MixSeed((unsigned int)monitor->rect.top + 0x9e3779b9U));
    for (int i = 0; i < 4; ++i) {
        seed = MixSeed(seed + 0x9e3779b9U);
        surface->seed[i] = (float)(seed >> 8) * (1.0f / 16777216.0f);
    }
    monitor->surface = surface;
    return TRUE;
}

void BlackHoleOverlay_Render(BlackHoleMonitor* monitor, ULONGLONG elapsedMs) {
    BlackHoleRenderer* renderer = monitor && monitor->session
        ? (BlackHoleRenderer*)monitor->session->renderer : NULL;
    BlackHoleSurface* surface = monitor ? (BlackHoleSurface*)monitor->surface : NULL;
    if (!renderer || !surface || !surface->target) return;
    D3D11_VIEWPORT viewport = {0, 0, (FLOAT)(monitor->rect.right - monitor->rect.left),
        (FLOAT)(monitor->rect.bottom - monitor->rect.top), 0, 1};
    BlackHoleConstants data = {{ (float)elapsedMs / 1000.0f,
        (float)monitor->session->desktopWidth, (float)monitor->session->desktopHeight,
        (float)elapsedMs / (float)BLACK_HOLE_DURATION_MS }, { (float)(monitor->rect.left - monitor->session->desktopRect.left),
        (float)(monitor->rect.top - monitor->session->desktopRect.top), viewport.Width, viewport.Height },
        { surface->seed[0], surface->seed[1], surface->seed[2], surface->seed[3] }};
    ID3D11DeviceContext_OMSetRenderTargets(renderer->context, 1, &surface->target, NULL);
    ID3D11DeviceContext_RSSetViewports(renderer->context, 1, &viewport);
    ID3D11DeviceContext_UpdateSubresource(renderer->context, (ID3D11Resource*)renderer->constants,
        0, NULL, &data, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(renderer->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(renderer->context, renderer->vertexShader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(renderer->context, renderer->pixelShader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(renderer->context, 0, 1, &renderer->snapshotView);
    ID3D11DeviceContext_PSSetSamplers(renderer->context, 0, 1, &renderer->sampler);
    ID3D11DeviceContext_PSSetConstantBuffers(renderer->context, 0, 1, &renderer->constants);
    ID3D11DeviceContext_Draw(renderer->context, 3, 0);
    IDXGISwapChain_Present(surface->swapChain, 1, 0);
}

void BlackHoleOverlay_ReleaseRenderer(BlackHoleSession* session) {
    if (!session) return;
    for (int i = 0; i < session->monitorCount; ++i) ReleaseSurface(&session->monitors[i]);
    ReleaseRenderer((BlackHoleRenderer*)session->renderer);
    session->renderer = NULL;
}