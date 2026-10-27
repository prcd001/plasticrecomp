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
 * around each texel edge, then blends the 4 nearest palette colours with those weights. Smooth is the
 * same blend with no band (plain bilinear on colours). Scanlines is sharp-bilinear with each source
 * row darkened towards its edges (only where a source row is at least 2 screen pixels tall). */
static const char k_hlsl[] =
"Texture2D<uint>   frame_idx : register(t0);\n"
"Texture2D<float4> palette   : register(t1);\n"
"cbuffer params : register(b0) {\n"
"    float2 origin; float2 inv_scale; int2 size; float prescale; int mode;\n"
"    int2 src_lo; int2 src_hi;\n"   /* the region's source rect: sampling stays inside it */
"};\n"
"float4 vs(uint id : SV_VertexID) : SV_Position {\n"
"    float2 p = float2((id << 1) & 2, id & 2);\n"
"    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float3 colour(int2 p) {\n"
"    p = clamp(p, src_lo, src_hi - 1);\n"
"    return palette.Load(int3((int)frame_idx.Load(int3(p, 0)), 0, 0)).rgb;\n"
"}\n"
"float4 ps(float4 pos : SV_Position) : SV_Target {\n"
"    float2 t = (pos.xy - origin) * inv_scale + float2(src_lo);\n"
"    if (mode == 0) return float4(colour(int2(floor(t))), 1);\n"
"    float2 tf = floor(t);\n"
"    float2 d = (t - tf) - 0.5;\n"
"    float  pre = (mode == 2) ? 1.0 : prescale;\n"
"    float  r = 0.5 - 0.5 / pre;\n"
"    float2 m = tf + (d - clamp(d, -r, r)) * pre;\n" /* = mod_texel - 0.5 */
"    float2 i0 = floor(m);\n"
"    float2 w = m - i0;\n"
"    int2 i = int2(i0);\n"
"    float3 a = colour(i),             b = colour(i + int2(1, 0));\n"
"    float3 c = colour(i + int2(0, 1)), e = colour(i + int2(1, 1));\n"
"    float3 o = lerp(lerp(a, b, w.x), lerp(c, e, w.x), w.y);\n"
"    if (mode == 3 && inv_scale.y <= 0.5) o *= 0.62 + 0.38 * sin((t.y - tf.y) * 3.14159265);\n"
"    return float4(o, 1);\n"
"}\n";

typedef struct {
    float origin[2], inv_scale[2];
    int   size[2];
    float prescale;
    int   mode;
    int   src_lo[2], src_hi[2];
} params_t;

/* The regions of the last presented frame, for present_window_to_frame. */
#define MAX_REGIONS 8
static present_region_t s_regions[MAX_REGIONS];
static int s_nregions;

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
static int  s_filter = PRESENT_SHARP;
static int  s_vsync = 1;
static char s_shot_dir[MAX_PATH];        /* a screenshot is pending when this is not empty */
static char s_shot_result[MAX_PATH];
static volatile LONG s_shot_new;
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
    s_filter = mode ? PRESENT_SHARP : PRESENT_NEAREST;
}

/* F12's order: sharp-bilinear first (the default), then nearest, smooth, scanlines. */
static const int k_cycle[PRESENT_NMODES] = { PRESENT_SHARP, PRESENT_NEAREST, PRESENT_SMOOTH, PRESENT_SCANLINES };
static const char* const k_names[PRESENT_NMODES] = { "nearest", "sharp-bilinear", "smooth bilinear", "scanlines" };

void present_set_mode(int mode)
{
    if (mode >= 0 && mode < PRESENT_NMODES) s_filter = mode;
}

int present_get_mode(void) { return s_filter; }

int present_cycle_mode(int dir)
{
    int i = 0;
    while (i < PRESENT_NMODES - 1 && k_cycle[i] != s_filter) i++;
    i = (i + (dir < 0 ? PRESENT_NMODES - 1 : 1)) % PRESENT_NMODES;
    s_filter = k_cycle[i];
    return s_filter;
}

const char* present_mode_name(int mode)
{
    return mode >= 0 && mode < PRESENT_NMODES ? k_names[mode] : "?";
}

int present_mode_from_name(const char* s)
{
    int i;
    if (!s || !*s) return -1;
    if (s[0] >= '0' && s[0] <= '9' && !s[1]) return s[0] - '0' < PRESENT_NMODES ? s[0] - '0' : -1;
    for (i = 0; i < PRESENT_NMODES; i++)
        if (!_strnicmp(s, k_names[i], 3)) return i;
    return -1;
}

double present_fit_scale(int cw, int ch, int w, int h)
{
    double s = (double)cw / w < (double)ch / h ? (double)cw / w : (double)ch / h;
    if (s_filter == PRESENT_NEAREST && s >= 1.0) s = floor(s + 1e-9);   /* integer scale: bars take the rest */
    return s;
}

void present_set_vsync(int on) { s_vsync = on ? 1 : 0; }
int  present_get_vsync(void) { return s_vsync; }

void present_request_screenshot(const char* dir)
{
    if (dir && *dir && !s_shot_dir[0]) strncpy(s_shot_dir, dir, MAX_PATH - 1);
}

int present_screenshot_result(char* out, int n)
{
    if (!InterlockedExchange(&s_shot_new, 0)) return 0;
    if (out && n > 0) { strncpy(out, s_shot_result, n - 1); out[n - 1] = 0; }
    return 1;
}

/* ---- PNG writer: 8-bit RGB, no filter, zlib "stored" blocks (no compressor needed) ---- */
static uint32_t crc_table[256];
static uint32_t crc_update(uint32_t c, const uint8_t* p, size_t n)
{
    size_t i;
    if (!crc_table[1]) {
        uint32_t k, j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[k] = v;
        }
    }
    for (i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n)
{
    uint8_t b[4];
    uint32_t c;
    put_be32(b, n); fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    c = crc_update(0xFFFFFFFFu, (const uint8_t*)type, 4);
    if (n) { fwrite(data, 1, n, f); c = crc_update(c, data, n); }
    put_be32(b, ~c); fwrite(b, 1, 4, f);
}
/* rgb: w*h*3 bytes. Returns 1 on success. */
static int write_png(const char* path, const uint8_t* rgb, int w, int h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    FILE* f = fopen(path, "wb");
    size_t rowb = (size_t)w * 3 + 1, rawn = rowb * h, off = 0;
    uint8_t* raw, *z, hdr[13];
    size_t zi = 0;
    uint32_t a = 1, b = 0;
    size_t i;
    if (!f) return 0;
    raw = (uint8_t*)malloc(rawn);
    z = (uint8_t*)malloc(rawn + (rawn / 65535 + 1) * 5 + 16);
    if (!raw || !z) { free(raw); free(z); fclose(f); return 0; }
    for (i = 0; i < (size_t)h; i++) { raw[i * rowb] = 0; memcpy(raw + i * rowb + 1, rgb + i * (size_t)w * 3, (size_t)w * 3); }
    z[zi++] = 0x78; z[zi++] = 0x01;
    while (off < rawn) {
        size_t n = rawn - off > 65535 ? 65535 : rawn - off;
        z[zi++] = (off + n == rawn) ? 1 : 0;
        z[zi++] = (uint8_t)n; z[zi++] = (uint8_t)(n >> 8); z[zi++] = (uint8_t)~n; z[zi++] = (uint8_t)(~n >> 8);
        memcpy(z + zi, raw + off, n); zi += n; off += n;
    }
    for (i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    put_be32(z + zi, (b << 16) | a); zi += 4;
    fwrite(sig, 1, 8, f);
    put_be32(hdr, (uint32_t)w); put_be32(hdr + 4, (uint32_t)h);
    hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
    png_chunk(f, "IHDR", hdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zi);
    png_chunk(f, "IEND", NULL, 0);
    i = (size_t)ferror(f);
    fclose(f);
    free(raw); free(z);
    return i == 0;
}

/* Read the back buffer (BGRA) into a PNG, before Present. Fills s_shot_result; clears the request. */
static void save_screenshot(void)
{
    ID3D11Texture2D *bb = NULL, *st = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    uint8_t* rgb = NULL;
    SYSTEMTIME t;
    char path[MAX_PATH];
    int ok = 0, x, y, n = 0;
    FILE* ex;

    CreateDirectoryA(s_shot_dir, NULL);
    GetLocalTime(&t);
    do {
        if (n) _snprintf(path, MAX_PATH - 1, "%s\\plastic_%04d%02d%02d_%02d%02d%02d_%d.png", s_shot_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
        else   _snprintf(path, MAX_PATH - 1, "%s\\plastic_%04d%02d%02d_%02d%02d%02d.png", s_shot_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        path[MAX_PATH - 1] = 0;
        ex = fopen(path, "rb");
        if (ex) fclose(ex);
    } while (ex && ++n < 100);

    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_sc, 0, &pr_IID_ID3D11Texture2D, (void**)&bb))) {
        ID3D11Texture2D_GetDesc(bb, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) {
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource*)st, (ID3D11Resource*)bb);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &map))) {
                rgb = (uint8_t*)malloc((size_t)td.Width * td.Height * 3);
                if (rgb) {
                    for (y = 0; y < (int)td.Height; y++) {
                        const uint8_t* sp = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
                        uint8_t* dp = rgb + (size_t)y * td.Width * 3;
                        for (x = 0; x < (int)td.Width; x++, sp += 4, dp += 3) dp[0] = sp[2], dp[1] = sp[1], dp[2] = sp[0];
                    }
                    ok = write_png(path, rgb, (int)td.Width, (int)td.Height);
                    free(rgb);
                }
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource*)st, 0);
            }
            RELEASE(st);
        }
        RELEASE(bb);
    }
    strncpy(s_shot_result, ok ? path : "", MAX_PATH - 1);
    s_shot_dir[0] = 0;
    InterlockedExchange(&s_shot_new, 1);
    fprintf(stderr, "[present] screenshot %s\n", ok ? path : "failed");
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
    present_frame_regions(pixels, pitch, w, h, pal, NULL, 0);
}

void present_frame_regions(const uint8_t* pixels, int pitch, int w, int h, const PALETTEENTRY pal[256],
                           const present_region_t* regions, int nregions)
{
    present_region_t whole;
    int r;
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

    /* Letterbox: largest w:h rectangle that fits (whole multiples only in nearest mode), centred on whole pixels. */
    scale = present_fit_scale(cw, ch, w, h);
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
    (void)prescale;

    /* No regions: the whole frame, letterboxed. */
    if (!regions || nregions <= 0) {
        whole.sx = 0, whole.sy = 0, whole.sw = w, whole.sh = h;
        whole.dx = s_dst_x, whole.dy = s_dst_y, whole.dw = dw, whole.dh = dh;
        regions = &whole;
        nregions = 1;
    }
    if (nregions > MAX_REGIONS) nregions = MAX_REGIONS;
    memcpy(s_regions, regions, (size_t)nregions * sizeof *regions);
    s_nregions = nregions;

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_rtv, black);
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
    /* One pass per region, each with its own scale: the viewport is the region's
     * destination, the shader maps it back into the region's source rect. */
    for (r = 0; r < nregions; r++) {
        const present_region_t* g = &regions[r];
        double rs;
        if (g->sw <= 0 || g->sh <= 0 || g->dw <= 0 || g->dh <= 0) continue;
        rs = (double)g->dw / g->sw;
        prescale = (int)floor(rs);
        if (prescale < 1) prescale = 1;
        prm.origin[0] = (float)g->dx;
        prm.origin[1] = (float)g->dy;
        prm.inv_scale[0] = (float)g->sw / g->dw;
        prm.inv_scale[1] = (float)g->sh / g->dh;
        prm.size[0] = w;
        prm.size[1] = h;
        prm.prescale = (float)prescale;
        prm.mode = s_filter;
        prm.src_lo[0] = g->sx;
        prm.src_lo[1] = g->sy;
        prm.src_hi[0] = g->sx + g->sw;
        prm.src_hi[1] = g->sy + g->sh;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource*)s_cb, 0, NULL, &prm, 0, 0);
        vp.TopLeftX = (float)g->dx;
        vp.TopLeftY = (float)g->dy;
        vp.Width = (float)g->dw;
        vp.Height = (float)g->dh;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    }

    if (s_shot_dir[0]) save_screenshot();
    hr = IDXGISwapChain_Present(s_sc, s_vsync ? 1 : 0, 0);
    if (lost(hr)) recover(hr);
    /* The flip model unbinds the back buffer on Present; it is bound again next frame. */
}

int present_window_to_frame(int cx, int cy, int* fx, int* fy)
{
    int x = 0, y = 0, inside = 0, r;
    /* Regions are drawn in order, so the last one containing the point is on top. */
    for (r = s_nregions - 1; r >= 0; r--) {
        const present_region_t* g = &s_regions[r];
        if (cx < g->dx || cx >= g->dx + g->dw || cy < g->dy || cy >= g->dy + g->dh) continue;
        x = g->sx + (int)floor(((cx - g->dx) + 0.5) * g->sw / g->dw);
        y = g->sy + (int)floor(((cy - g->dy) + 0.5) * g->sh / g->dh);
        if (x > g->sx + g->sw - 1) x = g->sx + g->sw - 1;
        if (y > g->sy + g->sh - 1) y = g->sy + g->sh - 1;
        if (fx) *fx = x;
        if (fy) *fy = y;
        return 1;
    }
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
