// Render thread: Desktop Duplication -> per-channel sub-pixel shift -> topmost overlay.
//
// The overlay is a click-through window covering the corrected display and excluded
// from screen capture, so duplicating the display never sees the overlay itself.
// Only areas that changed are copied and re-shaded; frames in which nothing visible
// changed (including the recomposition caused by our own Present) are detected on the
// GPU and skipped, which keeps the load proportional to what actually moves on screen.

#include "app.h"
#include "resource.h"
#include "shift_constants.h"

#include <d3d11.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

Runtime g_runtime;

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

// Input texels an output pixel reads beyond its own position: up to 1 px of integer
// shift plus the 4-tap kernel and the Laplacian reach 3 texels; 1 more for safety.
static const int kReadMargin = 4;

// Output area that still has to be redrawn from the current desktop copy.
struct RedrawRegion {
    std::vector<RECT> rects;
    bool  full = false;
    INT64 area = 0;

    bool Empty() const { return !full && rects.empty(); }
    void Clear() { full = false; rects.clear(); area = 0; }
    void AddFull() { full = true; rects.clear(); }
    // r is a changed input rect; every output pixel within kReadMargin of it can change.
    void AddChanged(RECT r, int w, int h) {
        if (full) return;
        r.left = std::max(0L, r.left - kReadMargin);
        r.top = std::max(0L, r.top - kReadMargin);
        r.right = std::min((LONG)w, r.right + kReadMargin);
        r.bottom = std::min((LONG)h, r.bottom + kReadMargin);
        if (r.left >= r.right || r.top >= r.bottom) return;
        rects.push_back(r);
        area += (INT64)(r.right - r.left) * (r.bottom - r.top);
        if (rects.size() > 32 || area > (INT64)w * h / 2) AddFull();
    }
};

struct Gpu {
    ID3D11Device*             dev = nullptr;
    ID3D11DeviceContext*      ctx = nullptr;
    IDXGISwapChain*           swap = nullptr;
    IDXGIOutputDuplication*   dup = nullptr;
    ID3D11Texture2D*          copy = nullptr;      // desktop image the overlay is rendered from
    ID3D11ShaderResourceView* copySrv = nullptr;
    ID3D11Texture2D*          incoming = nullptr;  // changed rects of the newest frame, before the diff
    ID3D11ShaderResourceView* incomingSrv = nullptr;
    std::vector<ID3D11Query*> queries;             // occlusion queries, one per changed rect
    ID3D11Texture2D*          back = nullptr;      // swap chain buffer
    ID3D11Texture2D*          out = nullptr;       // corrected image, kept between frames
    ID3D11RenderTargetView*   outRtv = nullptr;
    ID3D11RasterizerState*    scissor = nullptr;
    ID3D11VertexShader*       vs = nullptr;
    ID3D11PixelShader*        psCorrect = nullptr;
    ID3D11PixelShader*        psDiff = nullptr;
    ID3D11Buffer*             cb = nullptr;
    HWND                      overlay = nullptr;

    void ReleaseFrameResources() {
        SafeRelease(copySrv);
        SafeRelease(copy);
        SafeRelease(incomingSrv);
        SafeRelease(incoming);
    }
    void ReleaseAll() {
        ReleaseFrameResources();
        for (ID3D11Query*& q : queries) SafeRelease(q);
        queries.clear();
        SafeRelease(cb);
        SafeRelease(psDiff);
        SafeRelease(psCorrect);
        SafeRelease(vs);
        SafeRelease(scissor);
        SafeRelease(outRtv);
        SafeRelease(out);
        SafeRelease(back);
        SafeRelease(dup);
        SafeRelease(swap);
        SafeRelease(ctx);
        SafeRelease(dev);
        if (overlay) {
            DestroyWindow(overlay);
            overlay = nullptr;
        }
    }
};

static void NotifyPanel() {
    if (g_panel) PostMessageW(g_panel, WM_APP_STATE, 0, 0);
}

static HRESULT Duplicate(Gpu& g, const std::wstring& device) {
    if (!g.dev) return E_FAIL;
    if (IDXGIOutput1* out = FindOutput(device, nullptr)) {
        HRESULT hr = out->DuplicateOutput(g.dev, &g.dup);
        out->Release();
        if (SUCCEEDED(hr) && g.dup) return hr;
        SafeRelease(g.dup);
    }
    if (!TargetValid()) return E_FAIL;
    return g_displays[g_target].output->DuplicateOutput(g.dev, &g.dup);
}

static const char* ShaderSource(size_t* size) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(IDR_SHADER), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return nullptr;
    *size = SizeofResource(mod, res);
    return (const char*)LockResource(LoadResource(mod, res));
}

// Shaders are compiled at startup from the embedded HLSL source.
static bool CompileShaders(Gpu& g) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? (D3DCompileFn)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
    size_t size = 0;
    const char* src = ShaderSource(&size);
    if (!compile || !src) return false;
    auto build = [&](const char* entry, const char* target, ID3DBlob** blob) {
        ID3DBlob* errors = nullptr;
        HRESULT hr = compile(src, size, nullptr, nullptr, nullptr, entry, target, 0, 0, blob, &errors);
        SafeRelease(errors);
        return SUCCEEDED(hr);
    };
    ID3DBlob *vs = nullptr, *ps = nullptr, *diff = nullptr;
    bool ok = build("vsmain", "vs_4_0", &vs) && build("psmain", "ps_4_0", &ps) && build("diffmain", "ps_4_0", &diff) &&
              SUCCEEDED(g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs)) &&
              SUCCEEDED(g.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.psCorrect)) &&
              SUCCEEDED(g.dev->CreatePixelShader(diff->GetBufferPointer(), diff->GetBufferSize(), nullptr, &g.psDiff));
    SafeRelease(vs);
    SafeRelease(ps);
    SafeRelease(diff);
    return ok;
}

// Creates the overlay window and every D3D object for the target display.
static bool SetupTarget(Gpu& g, std::wstring& device, RECT& mon, int& w, int& h) {
    if (!TargetValid()) return false;
    const Display& d = g_displays[g_target];
    mon = d.rect;
    w = mon.right - mon.left;
    h = mon.bottom - mon.top;

    g.overlay = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                L"FringelessOverlay", APP_NAME, WS_POPUP, mon.left, mon.top, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!g.overlay) return false;
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(g.overlay, WDA_EXCLUDEFROMCAPTURE)) return false;

    device = d.device;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput1* out = FindOutput(device, &adapter);
    HRESULT hr = D3D11CreateDevice(adapter ? adapter : d.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
    SafeRelease(out);
    SafeRelease(adapter);
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    IDXGIFactory* factory = nullptr;
    g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    if (dxgiDev) dxgiDev->GetAdapter(&dxgiAdapter);
    if (dxgiAdapter) dxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
    hr = E_FAIL;
    if (factory) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = g.overlay;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(g.dev, &sd, &g.swap);
        factory->MakeWindowAssociation(g.overlay, DXGI_MWA_NO_ALT_ENTER);
    }
    SafeRelease(factory);
    SafeRelease(dxgiAdapter);
    SafeRelease(dxgiDev);
    if (FAILED(hr)) return false;

    // Draw into a persistent texture so only changed areas need shading; the swap chain
    // buffer (contents undefined after Present) receives a plain copy.
    if (FAILED(g.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g.back))) return false;
    D3D11_TEXTURE2D_DESC od = {};
    od.Width = w;
    od.Height = h;
    od.MipLevels = 1;
    od.ArraySize = 1;
    od.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    od.SampleDesc.Count = 1;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.dev->CreateTexture2D(&od, nullptr, &g.out))) return false;
    if (FAILED(g.dev->CreateRenderTargetView(g.out, nullptr, &g.outRtv))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(g.dev->CreateRasterizerState(&rd, &g.scissor))) return false;
    if (!CompileShaders(g)) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShaderConstants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;

    HRESULT dupHr = Duplicate(g, device);
    g_runtime.captureHr = dupHr;
    return SUCCEEDED(dupHr);
}

// Auto-pause: true when the foreground app is a game or video player covering the
// corrected display. Browsers, editors and terminals in full screen keep correction.
static bool ForegroundIsFullscreen(HWND overlay, const RECT& mon) {
    auto report = [](PauseReason r, bool pause) {
        g_runtime.reason = r;
        return pause;
    };

    QUERY_USER_NOTIFICATION_STATE quns;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns)) &&
        (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE))
        return report(PauseReason::ExclusiveFullscreen, true);

    HWND fg = GetForegroundWindow();
    if (!fg || fg == overlay || fg == g_panel) return report(PauseReason::NoForeground, false);
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 63);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return report(PauseReason::Shell, false);
    RECT r;
    if (!GetWindowRect(fg, &r)) return report(PauseReason::NoForeground, false);
    if (r.left > mon.left + 2 || r.top > mon.top + 2 || r.right < mon.right - 2 || r.bottom < mon.bottom - 2)
        return report(PauseReason::NotCovering, false);

    LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if (style & WS_MAXIMIZE) return report(PauseReason::Maximized, false);
    if ((style & WS_CAPTION) == WS_CAPTION) return report(PauseReason::HasCaption, false);

    std::wstring exe = ProcessNameOf(fg);
    if (!exe.empty()) {
        std::wstring list = L";" + g_settings.neverPause + L";";
        for (wchar_t& c : list) c = (wchar_t)towlower(c);
        if (list.find(L";" + exe + L";") != std::wstring::npos) return report(PauseReason::NeverPauseList, false);
    }
    return report(PauseReason::BorderlessFullscreen, true);
}

DWORD WINAPI RenderThread(void*) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FringelessOverlay";
    RegisterClassW(&wc);

    Gpu g;
    LARGE_INTEGER freq{}, lastRender{}, lastFrame{};
    QueryPerformanceFrequency(&freq);
    const double msPerTick = freq.QuadPart > 0 ? 1000.0 / (double)freq.QuadPart : 0.0;
    QueryPerformanceCounter(&lastFrame);
    auto ElapsedMs = [&](const LARGE_INTEGER& since) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - since.QuadPart) * msPerTick;
    };

    std::wstring device;
    RECT mon{};
    int W = 0, H = 0;
    int lastRetarget = -1;
    int lastEnabled = -1;
    bool paused = false;
    bool hiddenNoCapture = false;   // overlay hidden until capture works again
    ULONGLONG lastFsCheck = 0, lastDupAttempt = 0, lastTopmost = 0;
    int dupFailures = 0;
    INT64 lastPresentTime = 0;
    DXGI_FORMAT copyFormat = DXGI_FORMAT_UNKNOWN;
    bool formatWarned = false;
    RedrawRegion pending;
    int pendingThrottleMs = 0;
    std::vector<BYTE> meta;

    auto ShowOverlayTopmost = [&] {
        ShowWindow(g.overlay, SW_SHOWNA);
        SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOACTIVATE);
        g_runtime.redrawAll = true;
    };

    // Compares the changed rects of a new frame with the current copy on the GPU, takes
    // them over, and queues the ones that really differ for redraw.
    auto TakeChangedRects = [&](ID3D11Texture2D* tex, std::vector<RECT>& rects, UINT texW, UINT texH) {
        for (const RECT& r : rects) {
            D3D11_BOX box = { (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
            g.ctx->CopySubresourceRegion(g.incoming, 0, r.left, r.top, 0, tex, 0, &box);
        }
        while (g.queries.size() < rects.size()) {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
            ID3D11Query* q = nullptr;
            if (FAILED(g.dev->CreateQuery(&qd, &q))) break;
            g.queries.push_back(q);
        }
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)texW, (float)texH, 0.0f, 1.0f };
        ID3D11ShaderResourceView* views[2] = { g.copySrv, g.incomingSrv };
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->RSSetState(g.scissor);
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs, nullptr, 0);
        g.ctx->PSSetShader(g.psDiff, nullptr, 0);
        g.ctx->PSSetShaderResources(0, 2, views);
        for (size_t i = 0; i < rects.size() && i < g.queries.size(); ++i) {
            g.ctx->RSSetScissorRects(1, &rects[i]);
            g.ctx->Begin(g.queries[i]);
            g.ctx->Draw(3, 0);
            g.ctx->End(g.queries[i]);
        }
        ID3D11ShaderResourceView* none[2] = {};
        g.ctx->PSSetShaderResources(0, 2, none);
        for (const RECT& r : rects) {
            D3D11_BOX box = { (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
            g.ctx->CopySubresourceRegion(g.copy, 0, r.left, r.top, 0, g.incoming, 0, &box);
        }
        bool any = false;
        for (size_t i = 0; i < rects.size(); ++i) {
            UINT64 differing = 1;   // no query result: assume it changed
            if (i < g.queries.size()) {
                HRESULT qr;
                while ((qr = g.ctx->GetData(g.queries[i], &differing, sizeof(differing), 0)) == S_FALSE)
                    SwitchToThread();
                if (FAILED(qr)) differing = 1;
            }
            if (differing) {
                pending.AddChanged(rects[i], W, H);
                any = true;
            }
        }
        return any;
    };

    auto Draw = [&] {
        QueryPerformanceCounter(&lastRender);
        ShaderConstants c;
        ShiftSettings s;
        s.rx = g_settings.shift[kRed][kX];   s.ry = g_settings.shift[kRed][kY];
        s.gx = g_settings.shift[kGreen][kX]; s.gy = g_settings.shift[kGreen][kY];
        s.bx = g_settings.shift[kBlue][kX];  s.by = g_settings.shift[kBlue][kY];
        s.bicubic = g_settings.bicubic;
        FillShaderConstants(c, s, (float)W, (float)H);
        g.ctx->UpdateSubresource(g.cb, 0, nullptr, &c, 0, 0);

        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f };
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->RSSetState(g.scissor);
        g.ctx->OMSetRenderTargets(1, &g.outRtv, nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(g.vs, nullptr, 0);
        g.ctx->PSSetShader(g.psCorrect, nullptr, 0);
        g.ctx->PSSetShaderResources(0, 1, &g.copySrv);
        g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
        if (pending.full) {
            D3D11_RECT all = { 0, 0, W, H };
            g.ctx->RSSetScissorRects(1, &all);
            g.ctx->Draw(3, 0);
        } else {
            for (const RECT& r : pending.rects) {
                g.ctx->RSSetScissorRects(1, &r);
                g.ctx->Draw(3, 0);
            }
        }
        pending.Clear();
        ID3D11ShaderResourceView* none = nullptr;
        g.ctx->PSSetShaderResources(0, 1, &none);
        g.ctx->CopyResource(g.back, g.out);
        return g.swap->Present(1, 0);
    };

    // Takes one desktop frame and redraws the overlay where it changed.
    auto CaptureAndRender = [&] {
        ULONGLONG now = GetTickCount64();
        if (now - lastTopmost > 1999) {
            // Other topmost windows can end up above us; re-assert every 2 s.
            if (!IsWindowVisible(g.overlay)) ShowWindow(g.overlay, SW_SHOWNA);
            SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOREDRAW | SWP_NOACTIVATE);
            g_runtime.redrawAll = true;
            lastTopmost = now;
        }

        DXGI_OUTDUPL_FRAME_INFO fi = {};
        IDXGIResource* res = nullptr;
        HRESULT hr = g.dup->AcquireNextFrame(32, &fi, &res);
        bool changed = false;
        int throttleMs = 0;

        if (hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_ACCESS_LOST) {
            g.ReleaseFrameResources();
            SafeRelease(g.dup);
            lastDupAttempt = 0;
            return;
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            g.ReleaseAll();
            Sleep(500);
            lastRetarget = -1;
            return;
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            QueryPerformanceCounter(&lastFrame);
        } else {
            if (FAILED(hr) || !res) {
                Sleep(30);
                return;
            }

            // Move rects come first in the metadata, then dirty rects. A move's
            // destination is not repeated among the dirty rects, so both count.
            UINT moveCount = 0, dirtyCount = 0;
            INT64 dirtyArea = 0;
            bool metaOk = true;
            std::vector<RECT> changedRects;
            if (fi.TotalMetadataBufferSize) {
                meta.resize(fi.TotalMetadataBufferSize);
                UINT moveBytes = 0, dirtyBytes = 0;
                metaOk = SUCCEEDED(g.dup->GetFrameMoveRects(fi.TotalMetadataBufferSize,
                                                            (DXGI_OUTDUPL_MOVE_RECT*)meta.data(), &moveBytes)) &&
                         SUCCEEDED(g.dup->GetFrameDirtyRects(fi.TotalMetadataBufferSize - moveBytes,
                                                             (RECT*)(meta.data() + moveBytes), &dirtyBytes));
                moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                dirtyCount = dirtyBytes / sizeof(RECT);
                const DXGI_OUTDUPL_MOVE_RECT* moves = (const DXGI_OUTDUPL_MOVE_RECT*)meta.data();
                for (UINT i = 0; i < moveCount; ++i) changedRects.push_back(moves[i].DestinationRect);
                const RECT* rects = (const RECT*)(meta.data() + moveBytes);
                for (UINT i = 0; i < dirtyCount; ++i) {
                    changedRects.push_back(rects[i]);
                    dirtyArea += (INT64)(rects[i].right - rects[i].left) * (rects[i].bottom - rects[i].top);
                }
            }
            QueryPerformanceCounter(&lastFrame);

            bool newFrame;
            INT64 present = fi.LastPresentTime.QuadPart;
            if (present == 0 || present == lastPresentTime) {
                newFrame = moveCount != 0 || dirtyCount != 0;
            } else {
                newFrame = true;
                lastPresentTime = present;
            }

            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
            res->Release();
            if (tex) {
                D3D11_TEXTURE2D_DESC td;
                tex->GetDesc(&td);
                if (td.Format != DXGI_FORMAT_R10G10B10A2_UNORM && td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                    tex->Release();
                    g.dup->ReleaseFrame();
                    if (!formatWarned) {
                        wchar_t text[400];
                        swprintf(text, 400,
                                 L"This display is composited in a format " APP_NAME L" cannot correct "
                                 L"(DXGI_FORMAT %u).\n\nSDR desktops with 8 or 10 bits per channel are supported. "
                                 L"Turn HDR off for this display to use " APP_NAME L".",
                                 (unsigned)td.Format);
                        MessageBoxW(nullptr, text, APP_NAME, MB_ICONWARNING);
                    }
                    formatWarned = true;
                    g_settings.enabled = false;
                    NotifyPanel();
                    return;
                }
                g_runtime.format = td.Format;

                bool fresh = false;
                if (!g.copy || copyFormat != td.Format) {
                    g.ReleaseFrameResources();
                    D3D11_TEXTURE2D_DESC cd = td;
                    cd.Usage = D3D11_USAGE_DEFAULT;
                    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    cd.CPUAccessFlags = 0;
                    cd.MiscFlags = 0;
                    g.dev->CreateTexture2D(&cd, nullptr, &g.copy);
                    g.dev->CreateTexture2D(&cd, nullptr, &g.incoming);
                    copyFormat = td.Format;
                    if (g.copy) g.dev->CreateShaderResourceView(g.copy, nullptr, &g.copySrv);
                    if (g.incoming) g.dev->CreateShaderResourceView(g.incoming, nullptr, &g.incomingSrv);
                    fresh = true;
                }

                if (g.copy && fresh) {
                    g.ctx->CopyResource(g.copy, tex);
                    pending.AddFull();
                    changed = true;
                } else if (g.copy && g.incomingSrv && newFrame) {
                    std::vector<RECT> rects;
                    if (metaOk && changedRects.size() <= 64) {
                        for (RECT r : changedRects) {
                            r.left = std::max(0L, r.left);
                            r.top = std::max(0L, r.top);
                            r.right = std::min((LONG)td.Width, r.right);
                            r.bottom = std::min((LONG)td.Height, r.bottom);
                            if (r.left < r.right && r.top < r.bottom) rects.push_back(r);
                        }
                    }
                    if (rects.empty()) rects.push_back({ 0, 0, (LONG)td.Width, (LONG)td.Height });
                    changed = TakeChangedRects(tex, rects, td.Width, td.Height);
                    // Small updates (caret blink, typing) are rate-limited to ~30 fps.
                    INT64 full = (INT64)W * H;
                    if (changed && moveCount == 0 && dirtyCount != 0 && dirtyArea < full / 20) throttleMs = 33;
                }
                tex->Release();
            }
            g.dup->ReleaseFrame();
        }

        if (!g.copySrv) {
            Sleep(10);
            return;
        }

        bool redrawAll = g_runtime.redrawAll.exchange(false);
        if (redrawAll) pending.AddFull();
        if (!changed) throttleMs = pendingThrottleMs;
        if (pending.Empty()) return;
        pendingThrottleMs = throttleMs;
        if (pendingThrottleMs != 0 && !redrawAll) {
            LASTINPUTINFO li = { sizeof(li) };
            bool recentInput = GetLastInputInfo(&li) && GetTickCount() - li.dwTime < 250;
            if (!recentInput && msPerTick > 0.0 && ElapsedMs(lastRender) < (double)pendingThrottleMs) {
                Sleep(1);
                return;
            }
        }

        hr = Draw();
        if (hr == DXGI_STATUS_OCCLUDED) {
            Sleep(60);
            lastTopmost = 0;
        } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            if (g.dev) hr = g.dev->GetDeviceRemovedReason();
            g_runtime.captureHr = hr;
            g.ReleaseAll();
            Sleep(300);
            lastRetarget = -1;
        } else if (FAILED(hr)) {
            g_runtime.captureHr = hr;
            SafeRelease(g.dup);
            lastDupAttempt = 0;
        }
    };

    // Watchdog, then show-if-needed, then one capture/render step.
    auto Step = [&](bool needShow) {
        if (msPerTick > 0.0 && ElapsedMs(lastFrame) > 1500.0) {
            // AcquireNextFrame has not returned for 1.5 s: the duplication is wedged.
            if (!needShow) ShowWindow(g.overlay, SW_HIDE);
            g.ReleaseFrameResources();
            SafeRelease(g.dup);
            QueryPerformanceCounter(&lastFrame);
            lastDupAttempt = 0;
            hiddenNoCapture = true;
            return;
        }
        if (needShow) ShowOverlayTopmost();
        hiddenNoCapture = false;
        CaptureAndRender();
    };

    // No duplication: retry at most every 500 ms, rebuild everything after 6 failures.
    auto RecreateDuplication = [&] {
        ULONGLONG now = GetTickCount64();
        if (now - lastDupAttempt >= 500) {
            long before = g_runtime.captureHr;
            g_runtime.captureHr = Duplicate(g, device);
            if ((before < 0) != (g_runtime.captureHr < 0)) NotifyPanel();
            lastDupAttempt = now;
            if (!g.dup) ++dupFailures;
        }
        if (g.dup) {
            g.ReleaseFrameResources();
            dupFailures = 0;
            Step(true);
            return;
        }
        if (dupFailures > 5) {
            g.ReleaseAll();
            Sleep(300);
            dupFailures = 0;
            lastRetarget = -1;
        }
        Sleep(100);
        hiddenNoCapture = true;
    };

    while (g_runtime.running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_runtime.retarget != lastRetarget) {
            g.ReleaseAll();
            lastRetarget = g_runtime.retarget;
            if (!SetupTarget(g, device, mon, W, H)) {
                g.ReleaseAll();
                Sleep(300);
                continue;
            }
            lastEnabled = -1;
            NotifyPanel();
        }
        if (!g.overlay) {
            Sleep(100);
            continue;
        }

        int enabled = g_settings.enabled ? 1 : 0;
        if (enabled != lastEnabled) {
            ShowWindow(g.overlay, enabled ? SW_SHOWNA : SW_HIDE);
            g_runtime.redrawAll = true;
            hiddenNoCapture = false;
        }
        lastEnabled = enabled;
        if (!enabled) {
            Sleep(30);
            continue;
        }

        ULONGLONG now = GetTickCount64();
        bool wantPause = paused;
        if (now - lastFsCheck >= 400) {
            lastFsCheck = now;
            wantPause = ForegroundIsFullscreen(g.overlay, mon) && g_settings.pauseInFullscreen;
        }

        if (g_settings.pauseInFullscreen && wantPause) {
            if (!paused) {
                ShowWindow(g.overlay, SW_HIDE);
                g_runtime.paused = true;
                g.ReleaseFrameResources();
                SafeRelease(g.dup);
                NotifyPanel();
            }
            Sleep(200);
            paused = true;
            continue;
        }

        if (paused) {
            paused = false;
            g_runtime.paused = false;
            NotifyPanel();
            lastDupAttempt = 0;
            if (!g.dup) RecreateDuplication();
            else Step(true);
            continue;
        }

        if (!g.dup) {
            if (!hiddenNoCapture) ShowWindow(g.overlay, SW_HIDE);
            RecreateDuplication();
            continue;
        }
        Step(hiddenNoCapture);
    }

    g.ReleaseAll();
    return 0;
}
