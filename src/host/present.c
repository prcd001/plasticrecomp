/* present.c - Direct3D 11 presenter for 8-bit indexed frames (see present.h).
 *
 * One device, one swap chain on the caller's window (FLIP_DISCARD, else the DISCARD blt model),
 * an R8_UINT frame texture (DYNAMIC, Map WRITE_DISCARD each frame), a 256x1 RGBA8 palette
 * texture (PALETTEENTRY is R,G,B,flags in memory, so it uploads as is; rewritten only when the
 * palette changes) and one pixel shader, compiled at start-up with D3DCompile, that does the
 * index fetch, palette lookup and filtering. The viewport is the letterbox rectangle; the rest
 * of the back buffer is cleared to black.
 *
 * Environment: PLASTIC_PRESENT_WARP=1 skips the hardware device (tests the WARP path),
 * PLASTIC_PRESENT_BLT=1 skips the flip model (tests the DISCARD blt path). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

/* Local copies of the interface IDs, so neither dxguid.lib nor INITGUID is needed. */
static const IID pr_IID_IDXGIDevice =
    {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const IID pr_IID_IDXGIFactory =
    {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
static const IID pr_IID_IDXGIFactory2 =
    {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const IID pr_IID_ID3D11Texture2D =
    {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

/* Full-screen triangle; the pixel shader maps SV_Position (render-target pixels) to frame
 * texels: t = (pos - origin) * inv_scale. Nearest takes texel floor(t). Sharp-bilinear
 * (libretro interpolation/sharp-bilinear) squeezes the blend into a band of 1/prescale texel
 * around each texel edge, then blends the 4 nearest palette colours with those weights. */
static const char k_hlsl[] =
"Texture2D<uint>   frame_idx : register(t0);\n"
"Texture2D<float4> palette   : register(t1);\n"
"cbuffer params : register(b0) {\n"
"    float2 origin; float2 inv_scale; int2 size; float prescale; int mode;\n"
"};\n"
"float4 vs(uint id : SV_VertexID) : SV_Position {\n"
"    float2 p = float2((id << 1) & 2, id & 2);\n"
"    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float3 colour(int2 p) {\n"
"    p = clamp(p, int2(0, 0), size - 1);\n"
"    return palette.Load(int3((int)frame_idx.Load(int3(p, 0)), 0, 0)).rgb;\n"
"}\n"
"float4 ps(float4 pos : SV_Position) : SV_Target {\n"
"    float2 t = (pos.xy - origin) * inv_scale;\n"
"    if (mode == 0) return float4(colour(int2(floor(t))), 1);\n"
"    float2 tf = floor(t);\n"
"    float2 d = (t - tf) - 0.5;\n"
"    float  r = 0.5 - 0.5 / prescale;\n"
"    float2 m = tf + (d - clamp(d, -r, r)) * prescale;\n" /* = mod_texel - 0.5 */
"    float2 i0 = floor(m);\n"
"    float2 w = m - i0;\n"
"    int2 i = int2(i0);\n"
"    float3 a = colour(i),             b = colour(i + int2(1, 0));\n"
"    float3 c = colour(i + int2(0, 1)), e = colour(i + int2(1, 1));\n"
"    return float4(lerp(lerp(a, b, w.x), lerp(c, e, w.x), w.y), 1);\n"
"}\n";

typedef struct {
    float origin[2], inv_scale[2];
    int   size[2];
    float prescale;
    int   mode;
} params_t;

static HWND                     s_hwnd;
static ID3D11Device*            s_dev;
static ID3D11DeviceContext*     s_ctx;
static IDXGISwapChain*          s_sc;
static ID3D11RenderTargetView*  s_rtv;
static ID3D11VertexShader*      s_vs;
static ID3D11PixelShader*       s_ps;
static ID3D11Buffer*            s_cb;
static ID3D11Texture2D*         s_frame_tex;
static ID3D11ShaderResourceView* s_frame_srv;
static ID3D11Texture2D*         s_pal_tex;
static ID3D11ShaderResourceView* s_pal_srv;
static int  s_flip;              /* swap chain uses the flip model */
static int  s_bb_w, s_bb_h;      /* back buffer size */
static int  s_tex_w, s_tex_h;    /* frame texture size */
static int  s_filter = 1;
static PALETTEENTRY s_pal_cache[256];
static int  s_pal_valid;
/* Letterbox of the last presented frame, for present_window_to_frame. */
static int  s_last_w, s_last_h, s_dst_x, s_dst_y, s_dst_w, s_dst_h;

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

static int fail(const char* what, HRESULT hr)
{
    fprintf(stderr, "present: %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
    return 0;
}

static int env_on(const char* name)
{
    const char* v = getenv(name);
    return v && *v && *v != '0';
}

static HRESULT create_device(D3D_DRIVER_TYPE type)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
    HRESULT hr = D3D11CreateDevice(NULL, type, NULL, flags, levels, 4, D3D11_SDK_VERSION,
                                   &s_dev, NULL, &s_ctx);
    if (hr == E_INVALIDARG)  /* a D3D 11.0 runtime does not know 11_1 */
        hr = D3D11CreateDevice(NULL, type, NULL, flags, levels + 1, 3, D3D11_SDK_VERSION,
                               &s_dev, NULL, &s_ctx);
    return hr;
}

static int create_swap_chain(void)
{
    IDXGIDevice* dxdev = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory* factory = NULL;
    IDXGIFactory2* factory2 = NULL;
    RECT rc;
    HRESULT hr;

    GetClientRect(s_hwnd, &rc);
    s_bb_w = rc.right > 0 ? rc.right : 1;
    s_bb_h = rc.bottom > 0 ? rc.bottom : 1;

    hr = ID3D11Device_QueryInterface(s_dev, &pr_IID_IDXGIDevice, (void**)&dxdev);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxdev, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(adapter, &pr_IID_IDXGIFactory, (void**)&factory);
    RELEASE(adapter);
    RELEASE(dxdev);
    if (FAILED(hr)) return fail("getting the DXGI factory", hr);

    s_flip = 0;
    if (!env_on("PLASTIC_PRESENT_BLT") &&
        SUCCEEDED(IDXGIFactory_QueryInterface(factory, &pr_IID_IDXGIFactory2, (void**)&factory2))) {
        DXGI_SWAP_CHAIN_DESC1 d;
        IDXGISwapChain1* sc1 = NULL;
        memset(&d, 0, sizeof d);
        d.Width = s_bb_w;
        d.Height = s_bb_h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  /* Windows 10+ */
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory2, (IUnknown*)s_dev, s_hwnd, &d, NULL, NULL, &sc1);
        if (SUCCEEDED(hr)) {
            s_sc = (IDXGISwapChain*)sc1;  /* IDXGISwapChain1 derives from IDXGISwapChain */
            s_flip = 1;
        } else {
            fprintf(stderr, "present: FLIP_DISCARD swap chain failed (hr=0x%08lX), using DISCARD\n",
                    (unsigned long)hr);
        }
        RELEASE(factory2);
    }
    if (!s_sc) {
        DXGI_SWAP_CHAIN_DESC d;
        memset(&d, 0, sizeof d);
        d.BufferDesc.Width = s_bb_w;
        d.BufferDesc.Height = s_bb_h;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.OutputWindow = s_hwnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown*)s_dev, &d, &s_sc);
        if (FAILED(hr)) { RELEASE(factory); return fail("CreateSwapChain", hr); }
    }
    /* The game owns the window mode: no DXGI Alt+Enter, no DXGI message hooks. */
    IDXGIFactory_MakeWindowAssociation(factory, s_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    RELEASE(factory);
    return 1;
}

static int create_rtv(void)
{
    ID3D11Texture2D* bb = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(s_sc, 0, &pr_IID_ID3D11Texture2D, (void**)&bb);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource*)bb, NULL, &s_rtv);
    RELEASE(bb);
    return SUCCEEDED(hr) ? 1 : fail("creating the back buffer view", hr);
}

static int compile(const char* entry, const char* target, ID3DBlob** out)
{
    ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(k_hlsl, sizeof k_hlsl - 1, "present.hlsl", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "present: D3DCompile %s failed (hr=0x%08lX)\n%s\n", entry, (unsigned long)hr,
                err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
        RELEASE(err);
        return 0;
    }
    RELEASE(err);
    return 1;
}

static int create_pipeline(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (!compile("vs", "vs_4_0", &vsb) || !compile("ps", "ps_4_0", &psb)) {
        RELEASE(vsb);
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                            ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    RELEASE(vsb);
    RELEASE(psb);
    if (FAILED(hr)) return fail("creating the shaders", hr);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(params_t);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);
    if (FAILED(hr)) return fail("creating the constant buffer", hr);

    memset(&td, 0, sizeof td);
    td.Width = 256;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  /* PALETTEENTRY order: peRed, peGreen, peBlue, peFlags */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_pal_tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource*)s_pal_tex, NULL, &s_pal_srv);
    if (FAILED(hr)) return fail("creating the palette texture", hr);
    s_pal_valid = 0;
    return 1;
}

static int create_frame_texture(int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    RELEASE(s_frame_srv);
    RELEASE(s_frame_tex);
    s_tex_w = s_tex_h = 0;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_frame_tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource*)s_frame_tex, NULL, &s_frame_srv);
    if (FAILED(hr)) return fail("creating the frame texture", hr);
    s_tex_w = w;
    s_tex_h = h;
    return 1;
}

static void release_all(void)
{
    if (s_ctx) {
        ID3D11DeviceContext_ClearState(s_ctx);
        ID3D11DeviceContext_Flush(s_ctx);
    }
    RELEASE(s_frame_srv);
    RELEASE(s_frame_tex);
    RELEASE(s_pal_srv);
    RELEASE(s_pal_tex);
    RELEASE(s_cb);
    RELEASE(s_ps);
    RELEASE(s_vs);
    RELEASE(s_rtv);
    RELEASE(s_sc);
    RELEASE(s_ctx);
    RELEASE(s_dev);
    s_tex_w = s_tex_h = 0;
    s_pal_valid = 0;
}

int present_init(HWND hwnd)
{
    HRESULT hr = E_FAIL;
    if (s_dev) release_all();
    s_hwnd = hwnd;
    if (!IsWindow(hwnd)) { fprintf(stderr, "present: not a window\n"); return 0; }

    if (!env_on("PLASTIC_PRESENT_WARP")) {
        hr = create_device(D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(hr))
            fprintf(stderr, "present: hardware D3D11 device failed (hr=0x%08lX), trying WARP\n",
                    (unsigned long)hr);
    }
    if (!s_dev) {
        hr = create_device(D3D_DRIVER_TYPE_WARP);
        if (FAILED(hr)) return fail("D3D11CreateDevice (WARP)", hr);
    }
    if (!create_swap_chain() || !create_rtv() || !create_pipeline()) {
        release_all();
        return 0;
    }
    return 1;
}

void present_set_filter(int mode)
{
    s_filter = mode ? 1 : 0;
}

/* After DEVICE_REMOVED / DEVICE_RESET: build everything again on the same window. */
static void recover(HRESULT why)
{
    fprintf(stderr, "present: device lost (hr=0x%08lX), recreating\n", (unsigned long)why);
    if (s_dev) {
        HRESULT reason = ID3D11Device_GetDeviceRemovedReason(s_dev);
        fprintf(stderr, "present: removed reason 0x%08lX\n", (unsigned long)reason);
    }
    present_init(s_hwnd);
}

static int lost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

void present_frame(const uint8_t* pixels, int pitch, int w, int h, const PALETTEENTRY pal[256])
{
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp;
    params_t prm;
    static const float black[4] = {0, 0, 0, 1};
    RECT rc;
    int cw, ch, dw, dh, y, prescale;
    double scale;
    HRESULT hr;

    if (!s_dev || !pixels || w <= 0 || h <= 0) return;

    /* Follow the client size; a minimised (0x0) window presents nothing. */
    GetClientRect(s_hwnd, &rc);
    cw = rc.right;
    ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;
    if (cw != s_bb_w || ch != s_bb_h) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        RELEASE(s_rtv);
        hr = IDXGISwapChain_ResizeBuffers(s_sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            if (lost(hr)) recover(hr); else fail("ResizeBuffers", hr);
            return;
        }
        s_bb_w = cw;
        s_bb_h = ch;
        if (!create_rtv()) return;
    }

    /* Index texture. */
    if (w != s_tex_w || h != s_tex_h)
        if (!create_frame_texture(w, h)) return;
    hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource*)s_frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) {
        if (lost(hr)) recover(hr); else fail("Map frame", hr);
        return;
    }
    if ((int)map.RowPitch == pitch && pitch == w) {
        memcpy(map.pData, pixels, (size_t)w * h);
    } else {
        for (y = 0; y < h; y++)
            memcpy((uint8_t*)map.pData + (size_t)y * map.RowPitch, pixels + (size_t)y * pitch, w);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource*)s_frame_tex, 0);

    /* Palette texture, only when it changed. */
    if (pal && (!s_pal_valid || memcmp(pal, s_pal_cache, sizeof s_pal_cache) != 0)) {
        memcpy(s_pal_cache, pal, sizeof s_pal_cache);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource*)s_pal_tex, 0, NULL, s_pal_cache,
                                              sizeof s_pal_cache, 0);
        s_pal_valid = 1;
    }

    /* Letterbox: largest w:h rectangle that fits, centred on whole pixels. */
    scale = (double)cw / w < (double)ch / h ? (double)cw / w : (double)ch / h;
    dw = (int)(w * scale + 0.5);
    dh = (int)(h * scale + 0.5);
    if (dw > cw) dw = cw;
    if (dh > ch) dh = ch;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    s_dst_x = (cw - dw) / 2;
    s_dst_y = (ch - dh) / 2;
    s_dst_w = dw;
    s_dst_h = dh;
    s_last_w = w;
    s_last_h = h;
    prescale = (int)floor(scale);
    if (prescale < 1) prescale = 1;

    prm.origin[0] = (float)s_dst_x;
    prm.origin[1] = (float)s_dst_y;
    prm.inv_scale[0] = (float)w / dw;
    prm.inv_scale[1] = (float)h / dh;
    prm.size[0] = w;
    prm.size[1] = h;
    prm.prescale = (float)prescale;
    prm.mode = s_filter;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource*)s_cb, 0, NULL, &prm, 0, 0);

    /* Draw. */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_rtv, black);
    vp.TopLeftX = (float)s_dst_x;
    vp.TopLeftY = (float)s_dst_y;
    vp.Width = (float)dw;
    vp.Height = (float)dh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    {
        ID3D11ShaderResourceView* srvs[2];
        srvs[0] = s_frame_srv;
        srvs[1] = s_pal_srv;
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, srvs);
    }
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);

    hr = IDXGISwapChain_Present(s_sc, 1, 0);
    if (lost(hr)) recover(hr);
    /* The flip model unbinds the back buffer on Present; it is bound again next frame. */
}

int present_window_to_frame(int cx, int cy, int* fx, int* fy)
{
    int x = 0, y = 0, inside = 0;
    if (s_last_w > 0 && s_dst_w > 0 && s_dst_h > 0) {
        /* Same mapping as the shader: pixel centre (cx + 0.5) -> texel. */
        x = (int)floor(((cx - s_dst_x) + 0.5) * s_last_w / s_dst_w);
        y = (int)floor(((cy - s_dst_y) + 0.5) * s_last_h / s_dst_h);
        inside = cx >= s_dst_x && cx < s_dst_x + s_dst_w && cy >= s_dst_y && cy < s_dst_y + s_dst_h;
        if (x < 0) x = 0;
        if (x > s_last_w - 1) x = s_last_w - 1;
        if (y < 0) y = 0;
        if (y > s_last_h - 1) y = s_last_h - 1;
    }
    if (fx) *fx = x;
    if (fy) *fy = y;
    return inside;
}

void present_shutdown(void)
{
    release_all();
    s_hwnd = NULL;
    s_last_w = s_last_h = 0;
}
