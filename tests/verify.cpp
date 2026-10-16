// GPU tests for the correction pipeline (needs a Direct3D 11 GPU).
//
//  1. Correctness: src/shader.hlsl against a straightforward per-pixel CPU
//     implementation of the same filter, on a synthetic desktop (ClearType text,
//     UI-like blocks, a noisy photo area) for a range of settings.
//  2. Partial redraw: after changing a rect of the input, redrawing only that rect
//     grown by kReadMargin must give exactly the same output as a full redraw.
//  3. Change detection: the diff pass must count exactly the pixels that changed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/shift_constants.h"

static const int W = 1280, H = 720;
static const int kReadMargin = 4;   // must match src/renderer.cpp

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static int failures;

static void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printf("FAILED: %s (0x%08lX)\n", what, (unsigned long)hr);
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// CPU reference
// ---------------------------------------------------------------------------

struct Image {
    std::vector<uint32_t> px;   // BGRA
    float at(int x, int y, int ch) const {
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        uint32_t v = px[(size_t)y * W + x];
        int shift = ch == 0 ? 16 : ch == 1 ? 8 : 0;
        return ((v >> shift) & 0xFF) / 255.0f;
    }
};

static float Bilinear(const Image& im, float px, float py, int ch) {
    float tx = px - 0.5f, ty = py - 0.5f;
    float ix = std::floor(tx), iy = std::floor(ty);
    float fx = tx - ix, fy = ty - iy;
    int x = (int)ix, y = (int)iy;
    float top = im.at(x, y, ch) + fx * (im.at(x + 1, y, ch) - im.at(x, y, ch));
    float bottom = im.at(x, y + 1, ch) + fx * (im.at(x + 1, y + 1, ch) - im.at(x, y + 1, ch));
    return top + fy * (bottom - top);
}

static float Bicubic(const Image& im, float px, float py, int ch) {
    float tx = px - 0.5f, ty = py - 0.5f;
    float ix = std::floor(tx), iy = std::floor(ty);
    float wx[4], wy[4];
    CubicWeights(tx - ix, wx);
    CubicWeights(ty - iy, wy);
    float sum = 0.0f;
    for (int j = -1; j <= 2; ++j) {
        float row = 0.0f;
        for (int i = -1; i <= 2; ++i) row += wx[i + 1] * im.at((int)ix + i, (int)iy + j, ch);
        sum += wy[j + 1] * row;
    }
    return sum;
}

static float Laplacian(const Image& im, float px, float py, int ch) {
    int x = (int)std::floor(px), y = (int)std::floor(py);
    return im.at(x - 1, y, ch) + im.at(x + 1, y, ch) + im.at(x, y - 1, ch) + im.at(x, y + 1, ch) - 4.0f * im.at(x, y, ch);
}

// The correction, evaluated independently for every pixel and channel.
static std::vector<uint32_t> Reference(const Image& im, const ShiftSettings& s) {
    float sx[3] = { s.rx, s.gx, s.bx }, sy[3] = { s.ry, s.gy, s.by };
    float cx = -(std::min({ sx[0], sx[1], sx[2] }) + std::max({ sx[0], sx[1], sx[2] })) * 0.5f;
    float cy = -(std::min({ sy[0], sy[1], sy[2] }) + std::max({ sy[0], sy[1], sy[2] })) * 0.5f;
    float ca = s.bicubic ? 0.8625f : 1.7216f;
    std::vector<uint32_t> out((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float rgb[3];
            for (int ch = 0; ch < 3; ++ch) {
                float ux = sx[ch] + cx, uy = sy[ch] + cy;
                float nx = std::nearbyint(ux), ny = std::nearbyint(uy);
                float fx = ux - nx, fy = uy - ny;
                float px = x + 0.5f - nx - fx, py = y + 0.5f - ny - fy;
                float v = s.bicubic ? Bicubic(im, px, py, ch) : Bilinear(im, px, py, ch);
                float k = std::min(ca * (fx * fx + fy * fy), 0.35f);
                rgb[ch] = std::clamp(v - k * Laplacian(im, px, py, ch), 0.0f, 1.0f);
            }
            auto q = [](float v) { return (uint32_t)std::lround(v * 255.0f); };
            out[(size_t)y * W + x] = 0xFF000000u | q(rgb[0]) << 16 | q(rgb[1]) << 8 | q(rgb[2]);
        }
    return out;
}

// ---------------------------------------------------------------------------
// Test image
// ---------------------------------------------------------------------------

// Light and dark panels with ClearType text in several sizes and colours, coloured
// UI blocks, and a noisy "photo" area.
static Image MakeDesktop(unsigned seed) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(dc, bmp);
    uint32_t* px = (uint32_t*)bits;

    srand(seed);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) px[y * W + x] = x < W / 2 ? 0xF3F3F3 : 0x1E1E1E;
    for (int i = 0; i < 24; ++i) {
        RECT r = { rand() % W, rand() % H, 0, 0 };
        r.right = r.left + 30 + rand() % 200;
        r.bottom = r.top + 16 + rand() % 80;
        HBRUSH b = CreateSolidBrush(RGB(rand() % 256, rand() % 256, rand() % 256));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    for (int y = 450; y < 650; ++y)
        for (int x = 850; x < 1200; ++x)
            px[y * W + x] = (uint32_t)(rand() & 0xFF) << 16 | ((x * 7 + y) & 0xFF) << 8 | ((x ^ y) & 0xFF);

    SetBkMode(dc, TRANSPARENT);
    const char* words = "Sphinx of black quartz, judge my vow - lift, kilt, fizz, stilt 0123456789";
    for (int line = 0; line < 34; ++line) {
        LOGFONTA lf = {};
        lf.lfHeight = -(11 + (line % 6) * 3);
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfWeight = line % 4 == 0 ? FW_BOLD : FW_NORMAL;
        strcpy(lf.lfFaceName, line % 3 == 0 ? "Segoe UI" : line % 3 == 1 ? "Consolas" : "Calibri");
        HFONT f = CreateFontIndirectA(&lf);
        SelectObject(dc, f);
        SetTextColor(dc, line % 2 ? RGB(220, 220, 220) : RGB(rand() % 80, rand() % 80, rand() % 120));
        TextOutA(dc, line % 2 ? W / 2 + 20 : 20, 8 + line * 21, words, (int)strlen(words));
        DeleteObject(f);
    }
    GdiFlush();
    Image im;
    im.px.assign(px, px + W * H);
    for (uint32_t& p : im.px) p |= 0xFF000000u;
    DeleteObject(bmp);
    DeleteDC(dc);
    return im;
}

// ---------------------------------------------------------------------------
// GPU helpers
// ---------------------------------------------------------------------------

struct Shaders {
    ID3D11VertexShader* vs;
    ID3D11PixelShader* correct;
    ID3D11PixelShader* diff;
};

static Shaders Compile(const char* path) {
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = lib ? (D3DCompileFn)(void*)GetProcAddress(lib, "D3DCompile") : nullptr;
    if (!compile) { printf("d3dcompiler_47.dll not found\n"); exit(1); }
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(1); }
    std::string src;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
    fclose(f);
    auto build = [&](const char* entry, const char* target) {
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (FAILED(compile(src.data(), src.size(), path, nullptr, nullptr, entry, target, 0, 0, &blob, &err))) {
            printf("%s\n", err ? (const char*)err->GetBufferPointer() : "compile failed");
            exit(1);
        }
        return blob;
    };
    Shaders s;
    ID3DBlob* vs = build("vsmain", "vs_4_0");
    ID3DBlob* ps = build("psmain", "ps_4_0");
    ID3DBlob* df = build("diffmain", "ps_4_0");
    Check(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s.vs), "vs");
    Check(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s.correct), "ps");
    Check(dev->CreatePixelShader(df->GetBufferPointer(), df->GetBufferSize(), nullptr, &s.diff), "diff");
    return s;
}

static ID3D11Texture2D* MakeTex(UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = W;
    d.Height = H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    ID3D11Texture2D* t = nullptr;
    Check(dev->CreateTexture2D(&d, nullptr, &t), "CreateTexture2D");
    return t;
}

struct Input {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    void Upload(const Image& im, const RECT* r = nullptr) {
        if (r) {
            D3D11_BOX box = { (UINT)r->left, (UINT)r->top, 0, (UINT)r->right, (UINT)r->bottom, 1 };
            ctx->UpdateSubresource(tex, 0, &box, &im.px[(size_t)r->top * W + r->left], W * 4, 0);
        } else {
            ctx->UpdateSubresource(tex, 0, nullptr, im.px.data(), W * 4, 0);
        }
    }
};

static Input MakeInput() {
    Input in;
    in.tex = MakeTex(D3D11_BIND_SHADER_RESOURCE);
    Check(dev->CreateShaderResourceView(in.tex, nullptr, &in.srv), "srv");
    return in;
}

struct Target {
    ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv;
};

static Target MakeTarget() {
    Target t;
    t.tex = MakeTex(D3D11_BIND_RENDER_TARGET);
    Check(dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv), "rtv");
    return t;
}

static ID3D11RasterizerState* scissor;
static ID3D11Texture2D* readback;
static ID3D11Buffer* cb;
static Shaders sh;

static void Bind(ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* views, UINT count, ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(scissor);
    ctx->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(sh.vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, count, views);
    ctx->PSSetConstantBuffers(0, 1, &cb);
}

static void Unbind() {
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
}

static void Correct(const Input& in, const Target& t, const std::vector<RECT>& rects) {
    Bind(sh.correct, &in.srv, 1, t.rtv);
    for (const RECT& r : rects) {
        ctx->RSSetScissorRects(1, &r);
        ctx->Draw(3, 0);
    }
    Unbind();
}

static std::vector<uint32_t> Read(const Target& t) {
    ctx->CopyResource(readback, t.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    Check(ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<uint32_t> out((size_t)W * H);
    for (int y = 0; y < H; ++y) memcpy(&out[(size_t)y * W], (const BYTE*)m.pData + (size_t)y * m.RowPitch, W * 4);
    ctx->Unmap(readback, 0);
    for (uint32_t& p : out) p |= 0xFF000000u;
    return out;
}

struct Diff { int pixels = 0; int maxLevel = 0; };

static Diff Compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    Diff d;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        ++d.pixels;
        for (int s = 0; s < 24; s += 8)
            d.maxLevel = std::max(d.maxLevel, std::abs((int)((a[i] >> s) & 0xFF) - (int)((b[i] >> s) & 0xFF)));
    }
    return d;
}

static void SetConstants(const ShiftSettings& s) {
    ShaderConstants c;
    FillShaderConstants(c, s, (float)W, (float)H);
    ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
}

// Pixels the diff pass reports as different between two inputs inside r.
static UINT64 CountChanged(const Input& before, const Input& after, const RECT& r) {
    ID3D11Query* q = nullptr;
    D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
    Check(dev->CreateQuery(&qd, &q), "CreateQuery");
    ID3D11ShaderResourceView* views[2] = { before.srv, after.srv };
    Bind(sh.diff, views, 2, nullptr);
    ctx->RSSetScissorRects(1, &r);
    ctx->Begin(q);
    ctx->Draw(3, 0);
    ctx->End(q);
    Unbind();
    UINT64 n = 0;
    while (ctx->GetData(q, &n, sizeof(n), 0) == S_FALSE) SwitchToThread();
    q->Release();
    return n;
}

static void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        printf("  FAIL: %s\n", what);
    }
}

int main() {
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                            &ctx), "D3D11CreateDevice");
    sh = Compile("src/shader.hlsl");
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    Check(dev->CreateRasterizerState(&rd, &scissor), "rasterizer");
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShaderConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Check(dev->CreateBuffer(&bd, nullptr, &cb), "cbuffer");
    readback = MakeTex(0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);

    Image desktop = MakeDesktop(1);
    Input input = MakeInput();
    input.Upload(desktop);
    Target out = MakeTarget(), full = MakeTarget();
    const std::vector<RECT> all = { { 0, 0, W, H } };

    struct Case { const char* name; ShiftSettings s; };
    const Case cases[] = {
        { "vertical shifts only, bicubic",      { 0, -0.05f, 0, 0.10f, 0, -0.05f, true } },
        { "vertical shifts only, bilinear",     { 0, -0.05f, 0, 0.10f, 0, -0.05f, false } },
        { "both axes per channel, bilinear",    { -0.253f, 0.011f, 0, 0.593f, 0, -0.066f, false } },
        { "both axes per channel, bicubic",     { -0.253f, 0.011f, 0, 0.593f, 0, -0.066f, true } },
        { "odd values, bicubic",                { 0.37f, -0.81f, 0.123f, 0.5f, -1.0f, 0.999f, true } },
        { "half-pixel ties",                    { 0.5f, -0.5f, -0.5f, 0.5f, 0.25f, -0.75f, true } },
        { "zero shifts",                        { 0, 0, 0, 0, 0, 0, true } },
        { "extremes +-1 px",                    { 1, -1, -1, 1, 1, -1, true } },
    };
    printf("== 1. shader vs CPU reference (%dx%d) ==\n", W, H);
    for (const Case& k : cases) {
        SetConstants(k.s);
        Correct(input, out, all);
        Diff d = Compare(Reference(desktop, k.s), Read(out));
        printf("  %-36s differing pixels %6d (%.3f%%)  max diff %d/255\n", k.name, d.pixels, 100.0 * d.pixels / (W * H),
               d.maxLevel);
        Expect(d.maxLevel <= 1, k.name);
    }

    printf("== 2. partial redraw vs full redraw ==\n");
    Image next = MakeDesktop(7);
    const RECT changes[] = {
        { 500, 250, 700, 270 }, { 0, 0, 150, 100 }, { W - 64, H - 1, W, H }, { 640, 360, 641, 361 }, { 800, 420, 1250, 680 },
    };
    for (const Case& k : { cases[2], cases[4], cases[7] }) {
        SetConstants(k.s);
        for (const RECT& r : changes) {
            input.Upload(desktop);
            Correct(input, out, all);          // output of the previous frame
            input.Upload(next, &r);            // the change
            RECT grown = { std::max(0L, r.left - kReadMargin), std::max(0L, r.top - kReadMargin),
                           std::min((LONG)W, r.right + kReadMargin), std::min((LONG)H, r.bottom + kReadMargin) };
            Correct(input, out, { grown });    // partial redraw
            Correct(input, full, all);         // reference: full redraw
            Diff d = Compare(Read(full), Read(out));
            printf("  %-36s rect %4ld,%3ld-%4ld,%3ld  differing pixels %d\n", k.name, r.left, r.top, r.right, r.bottom,
                   d.pixels);
            Expect(d.pixels == 0, "partial redraw");
        }
    }

    printf("== 3. change detection ==\n");
    Input other = MakeInput();
    input.Upload(desktop);
    other.Upload(desktop);
    UINT64 n = CountChanged(input, other, all[0]);
    printf("  identical frames: %llu changed pixels\n", (unsigned long long)n);
    Expect(n == 0, "identical frames");
    Image one = desktop;
    one.px[(size_t)300 * W + 400] ^= 0x00000001u;   // one channel, one level
    other.Upload(one);
    n = CountChanged(input, other, all[0]);
    printf("  one pixel off by 1/255: %llu changed pixels\n", (unsigned long long)n);
    Expect(n == 1, "single pixel");
    RECT elsewhere = { 0, 0, 100, 100 };
    n = CountChanged(input, other, elsewhere);
    printf("  same change, outside the checked rect: %llu changed pixels\n", (unsigned long long)n);
    Expect(n == 0, "outside rect");
    RECT block = { 100, 100, 300, 150 };
    other.Upload(next, &block);
    n = CountChanged(input, other, all[0]);
    Diff expected = Compare(desktop.px, [&] {
        std::vector<uint32_t> v = one.px;
        for (int y = block.top; y < block.bottom; ++y)
            for (int x = block.left; x < block.right; ++x) v[(size_t)y * W + x] = next.px[(size_t)y * W + x];
        return v;
    }());
    printf("  changed block: %llu changed pixels (expected %d)\n", (unsigned long long)n, expected.pixels);
    Expect((int)n == expected.pixels, "changed block");

    printf("\n%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
