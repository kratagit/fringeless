// Built-in test pattern: real GDI/ClearType text in many fonts and sizes, for judging the
// colour fringe Fringeless corrects. The text must be drawn by the system (GDI) to carry
// ClearType's subpixel colour. The window repaints continuously so DWM keeps recompositing
// it — otherwise a static window would not show the correction turning on or off until it
// was forced to redraw (minimised and restored, or covered by another window).

#include "app.h"
#include "resource.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <vector>
#include <string>

namespace {

const wchar_t* kFonts[] = {
    L"Segoe UI", L"Segoe UI Semibold", L"Arial", L"Tahoma", L"Verdana", L"Calibri",
    L"Trebuchet MS", L"Times New Roman", L"Georgia", L"Cambria", L"Consolas",
    L"Courier New", L"Comic Sans MS",
};
const int kSizes[] = { 9, 10, 11, 12, 14, 18, 24 };
const wchar_t* kSample =
    L"Lorem ipsum dolor sit amet, consectetur adipiscing elit — illi 1l0O ij fj 0123456789";

// Chrome colours follow the system light/dark theme and accent, like the main window.
COLORREF C_CHROME, C_CHIP, C_CHIPH, C_ACC, C_ON = RGB(63, 209, 107), C_TXT, C_MUT, C_EDGE;
bool g_themeDark = true;

COLORREF SysAccent(bool dark) {
    BYTE pal[32]; DWORD sz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &sz) == ERROR_SUCCESS && sz >= 32) {
        const BYTE* c = pal + (dark ? 4 : 16);     // Light2 on dark, Dark1 on light
        return RGB(c[0], c[1], c[2]);
    }
    return dark ? RGB(0x60, 0xCD, 0xFF) : RGB(0x00, 0x5F, 0xB8);
}
void LoadTestTheme() {
    DWORD light = 1, s = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &s);
    g_themeDark = (light == 0);
    C_ACC = SysAccent(g_themeDark);
    if (g_themeDark) {
        C_CHROME = RGB(32, 32, 32); C_CHIP = RGB(45, 45, 45); C_CHIPH = RGB(56, 56, 56);
        C_TXT = RGB(255, 255, 255); C_MUT = RGB(165, 165, 165); C_EDGE = RGB(60, 60, 60);
    } else {
        C_CHROME = RGB(243, 243, 243); C_CHIP = RGB(251, 251, 251); C_CHIPH = RGB(237, 237, 237);
        C_TXT = RGB(26, 26, 26); C_MUT = RGB(96, 96, 96); C_EDGE = RGB(214, 214, 214);
    }
}

int g_scroll = 0, g_contentH = 0;
int g_spec = 0;    // 0 dark, 1 light, 2 grey specimen background
int g_gray = 0;
int g_mx = -1, g_my = -1, g_hot = -1;
HBITMAP g_back = nullptr; HDC g_backDC = nullptr; int g_backW = 0, g_backH = 0;

const int TOPBAR = 92, PADX = 20;
HWND g_wnd = nullptr;

struct Chip { RECT r; int grp; int val; };
std::vector<Chip> g_chips;
enum { G_BG = 0, G_MODE, G_CORR };

int Pt(HDC dc, int pt) { return -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72); }
void specColors(COLORREF* bg, COLORREF* fg, COLORREF* head) {
    if (g_spec == 1) { *bg = RGB(255, 255, 255); *fg = RGB(14, 14, 14); *head = RGB(0, 110, 130); }
    else if (g_spec == 2) { *bg = RGB(128, 128, 128); *fg = RGB(10, 10, 10); *head = RGB(0, 55, 75); }
    else { *bg = RGB(0, 0, 0); *fg = RGB(228, 228, 228); *head = RGB(95, 205, 225); }
}

void FreeBack() { if (g_backDC) { DeleteDC(g_backDC); g_backDC = nullptr; } if (g_back) { DeleteObject(g_back); g_back = nullptr; } }

void BuildBackbuffer(HDC ref, int w) {
    FreeBack(); if (w < 120) w = 120;
    HDC tmp = CreateCompatibleDC(ref);
    int y = 12;
    for (const wchar_t* fn : kFonts) {
        (void)fn; y += 28;
        for (int s : kSizes) {
            LOGFONTW lf{}; lf.lfHeight = Pt(ref, s); wcsncpy(lf.lfFaceName, L"Segoe UI", 31);
            HFONT f = CreateFontIndirectW(&lf); HGDIOBJ of = SelectObject(tmp, f);
            TEXTMETRICW tm; GetTextMetricsW(tmp, &tm); y += tm.tmHeight + tm.tmExternalLeading + 6;
            SelectObject(tmp, of); DeleteObject(f);
        }
        y += 14;
    }
    DeleteDC(tmp);
    int h = y + 20;

    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_backDC = CreateCompatibleDC(ref);
    g_back = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(g_backDC, g_back); g_backW = w; g_backH = h;

    COLORREF bg, fg, head; specColors(&bg, &fg, &head);
    HBRUSH b = CreateSolidBrush(bg); RECT all{ 0, 0, w, h }; FillRect(g_backDC, &all, b); DeleteObject(b);
    SetBkMode(g_backDC, OPAQUE); SetBkColor(g_backDC, bg);

    y = 12;
    for (const wchar_t* fn : kFonts) {
        LOGFONTW hl{}; hl.lfHeight = Pt(ref, 10); hl.lfWeight = FW_SEMIBOLD;
        wcscpy(hl.lfFaceName, L"Segoe UI"); hl.lfQuality = CLEARTYPE_QUALITY;
        HFONT hf = CreateFontIndirectW(&hl); HGDIOBJ ohf = SelectObject(g_backDC, hf);
        SetTextColor(g_backDC, head); TextOutW(g_backDC, PADX, y, fn, (int)wcslen(fn));
        SelectObject(g_backDC, ohf); DeleteObject(hf);
        y += 28;
        SetTextColor(g_backDC, fg);
        for (int s : kSizes) {
            LOGFONTW lf{}; lf.lfHeight = Pt(ref, s); lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = g_gray ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY;
            wcsncpy(lf.lfFaceName, fn, 31);
            HFONT f = CreateFontIndirectW(&lf); HGDIOBJ of = SelectObject(g_backDC, f);
            std::wstring line = std::to_wstring(s) + L"px   " + kSample;
            TextOutW(g_backDC, PADX + 10, y, line.c_str(), (int)line.size());
            TEXTMETRICW tm; GetTextMetricsW(g_backDC, &tm); y += tm.tmHeight + tm.tmExternalLeading + 6;
            SelectObject(g_backDC, of); DeleteObject(f);
        }
        y += 14;
    }
    g_contentH = h;
}

int ViewH(HWND w) { RECT r; GetClientRect(w, &r); int v = r.bottom - TOPBAR; return v > 0 ? v : 0; }
void ClampScroll(HWND w) {
    int maxS = g_contentH - ViewH(w); if (maxS < 0) maxS = 0;
    g_scroll = g_scroll > maxS ? maxS : g_scroll; if (g_scroll < 0) g_scroll = 0;
}

bool g_sbDrag = false; int g_sbDragY0 = 0, g_sbDragS0 = 0; bool g_sbHot = false;

// A thin Windows 11-style scrollbar drawn into the client area (the classic non-client
// scrollbar can't follow the dark theme). Returns false when the content fits.
bool SBMetrics(HWND w, RECT* track, RECT* thumb) {
    RECT cr; GetClientRect(w, &cr);
    int vh = ViewH(w); if (g_contentH <= vh || vh <= 0) return false;
    int sbW = 11, tx = cr.right - sbW - 3;
    track->left = tx; track->right = tx + sbW; track->top = TOPBAR + 3; track->bottom = cr.bottom - 3;
    int trackH = track->bottom - track->top;
    int thumbH = (int)((double)vh / g_contentH * trackH); if (thumbH < 30) thumbH = 30;
    int maxS = g_contentH - vh;
    int thumbY = track->top + (maxS ? (int)((double)g_scroll / maxS * (trackH - thumbH)) : 0);
    thumb->left = tx + 2; thumb->right = tx + sbW - 2; thumb->top = thumbY; thumb->bottom = thumbY + thumbH;
    return true;
}

HFONT uiFont(HDC dc, int pt, int weight) {
    LOGFONTW lf{}; lf.lfHeight = Pt(dc, pt); lf.lfWeight = weight; lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI"); return CreateFontIndirectW(&lf);
}

int DrawChip(HDC hdc, int x, int y, const wchar_t* label, bool sel, COLORREF selColor, int grp, int val) {
    SIZE sz; GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &sz);
    int w = sz.cx + 24, h = 30;
    bool hot = (g_hot == (int)g_chips.size());
    HBRUSH fill = CreateSolidBrush(sel ? selColor : (hot ? C_CHIPH : C_CHIP));
    HPEN pen = CreatePen(PS_SOLID, 1, sel ? selColor : C_EDGE);
    HGDIOBJ ob = SelectObject(hdc, fill), op = SelectObject(hdc, pen);
    RoundRect(hdc, x, y, x + w, y + h, 9, 9);
    SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(fill); DeleteObject(pen);
    int lum = (GetRValue(selColor) * 299 + GetGValue(selColor) * 587 + GetBValue(selColor) * 114) / 1000;
    SetTextColor(hdc, sel ? (lum > 140 ? RGB(0, 0, 0) : RGB(255, 255, 255)) : C_TXT);
    RECT tr{ x, y, x + w, y + h }; DrawTextW(hdc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    g_chips.push_back({ { x, y, x + w, y + h }, grp, val });
    return x + w + 8;
}
int Label(HDC hdc, int x, int y, const wchar_t* t) {
    SetTextColor(hdc, C_MUT); TextOutW(hdc, x, y + 7, t, (int)wcslen(t));
    SIZE sz; GetTextExtentPoint32W(hdc, t, (int)wcslen(t), &sz); return x + sz.cx + 12;
}

void Paint(HWND w) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(w, &ps);
    RECT cr; GetClientRect(w, &cr); int W = cr.right;
    if (!g_back || g_backW != W) { BuildBackbuffer(hdc, W); ClampScroll(w); }

    HBRUSH cb = CreateSolidBrush(C_CHROME); RECT tbr{ 0, 0, W, TOPBAR }; FillRect(hdc, &tbr, cb); DeleteObject(cb);
    SetBkMode(hdc, TRANSPARENT);
    g_chips.clear();

    HFONT title = uiFont(hdc, 12, FW_SEMIBOLD); HGDIOBJ oft = SelectObject(hdc, title);
    SetTextColor(hdc, C_TXT); TextOutW(hdc, PADX, 14, L"Test pattern", 12);
    SelectObject(hdc, oft); DeleteObject(title);

    HFONT ui = uiFont(hdc, 9, FW_NORMAL); HGDIOBJ ofu = SelectObject(hdc, ui);
    SetTextColor(hdc, C_MUT);
    const wchar_t* hint = L"Toggle correction in the app or tray — this window refreshes live.";
    TextOutW(hdc, PADX + 110, 17, hint, (int)wcslen(hint));

    int x, y = 48;
    x = Label(hdc, PADX, y, L"Background");
    x = DrawChip(hdc, x, y, L"Dark",  g_spec == 0, C_ACC, G_BG, 0);
    x = DrawChip(hdc, x, y, L"Light", g_spec == 1, C_ACC, G_BG, 1);
    x = DrawChip(hdc, x, y, L"Grey",  g_spec == 2, C_ACC, G_BG, 2);
    x = Label(hdc, x + 8, y, L"Text");
    x = DrawChip(hdc, x, y, L"ClearType", g_gray == 0, C_ACC, G_MODE, 0);
    x = DrawChip(hdc, x, y, L"Greyscale", g_gray == 1, C_ACC, G_MODE, 1);

    SelectObject(hdc, ofu); DeleteObject(ui);

    HPEN sep = CreatePen(PS_SOLID, 1, C_EDGE); HGDIOBJ os = SelectObject(hdc, sep);
    MoveToEx(hdc, 0, TOPBAR - 1, nullptr); LineTo(hdc, W, TOPBAR - 1);
    SelectObject(hdc, os); DeleteObject(sep);

    int vh = ViewH(w);
    if (g_back && vh > 0) {
        BitBlt(hdc, 0, TOPBAR, W, vh, g_backDC, 0, g_scroll, SRCCOPY);
        if (g_contentH - g_scroll < vh) {
            COLORREF bg, fg, hd; specColors(&bg, &fg, &hd);
            HBRUSH pad = CreateSolidBrush(bg); RECT t{ 0, TOPBAR + (g_contentH - g_scroll), W, cr.bottom };
            FillRect(hdc, &t, pad); DeleteObject(pad);
        }
    }

    RECT tr, th;
    if (SBMetrics(w, &tr, &th)) {
        COLORREF tc = (g_sbHot || g_sbDrag) ? C_MUT : (g_themeDark ? RGB(78, 78, 78) : RGB(194, 194, 194));
        HBRUSH thumb = CreateSolidBrush(tc);
        HGDIOBJ ob = SelectObject(hdc, thumb), op = SelectObject(hdc, GetStockObject(NULL_PEN));
        RoundRect(hdc, th.left, th.top, th.right, th.bottom, 6, 6);
        SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(thumb);
    }
    EndPaint(w, &ps);
}

int ChipAt(int px, int py) {
    for (int i = 0; i < (int)g_chips.size(); i++) {
        RECT& r = g_chips[i].r;
        if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) return i;
    }
    return -1;
}

LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: SetTimer(w, 1, 90, nullptr); return 0;  // keep the region live for the correction
    case WM_TIMER: { RECT r; GetClientRect(w, &r); r.top = TOPBAR; InvalidateRect(w, &r, FALSE); return 0; }
    case WM_LBUTTONDOWN: {
        int px = GET_X_LPARAM(lp), py = GET_Y_LPARAM(lp);
        RECT tr, th;
        if (SBMetrics(w, &tr, &th) && px >= tr.left && px < tr.right && py >= tr.top && py < tr.bottom) {
            if (py >= th.top && py < th.bottom) { g_sbDrag = true; g_sbDragY0 = py; g_sbDragS0 = g_scroll; SetCapture(w); }
            else { g_scroll += (py < th.top ? -1 : 1) * ViewH(w); ClampScroll(w); InvalidateRect(w, nullptr, FALSE); }
            return 0;
        }
        int i = ChipAt(px, py);
        if (i >= 0) {
            Chip& c = g_chips[i];
            if (c.grp == G_BG) { g_spec = c.val; g_backW = 0; }
            else if (c.grp == G_MODE) { g_gray = c.val; g_backW = 0; }
            InvalidateRect(w, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: if (g_sbDrag) { g_sbDrag = false; ReleaseCapture(); InvalidateRect(w, nullptr, FALSE); } return 0;
    case WM_MOUSEMOVE: {
        g_mx = GET_X_LPARAM(lp); g_my = GET_Y_LPARAM(lp);
        if (g_sbDrag) {
            RECT tr, th; SBMetrics(w, &tr, &th);
            int denom = (tr.bottom - tr.top) - (th.bottom - th.top); if (denom < 1) denom = 1;
            g_scroll = g_sbDragS0 + (int)((double)(g_my - g_sbDragY0) / denom * (g_contentH - ViewH(w)));
            ClampScroll(w); InvalidateRect(w, nullptr, FALSE); return 0;
        }
        RECT tr, th; bool sbHot = SBMetrics(w, &tr, &th) && g_mx >= th.left && g_mx < th.right && g_my >= th.top && g_my < th.bottom;
        if (sbHot != g_sbHot) { g_sbHot = sbHot; InvalidateRect(w, nullptr, FALSE); }
        int h = (g_my < TOPBAR) ? ChipAt(g_mx, g_my) : -1;
        if (h != g_hot) { g_hot = h; RECT tbr{ 0, 0, 0, TOPBAR }; GetClientRect(w, &tbr); tbr.bottom = TOPBAR; InvalidateRect(w, &tbr, FALSE); }
        SetCursor(LoadCursor(nullptr, (g_my < TOPBAR && g_hot >= 0) ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSEWHEEL: g_scroll -= GET_WHEEL_DELTA_WPARAM(wp); ClampScroll(w); InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_SIZE: g_backW = 0; ClampScroll(w); InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet")) {
            LoadTestTheme();
            BOOL d = g_themeDark; DwmSetWindowAttribute(w, 20, &d, sizeof(d));
            InvalidateRect(w, nullptr, TRUE);
        }
        return 0;
    case WM_PAINT: Paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: KillTimer(w, 1); FreeBack(); g_wnd = nullptr; return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

} // namespace

void OpenTestPattern() {
    if (g_wnd) { ShowWindow(g_wnd, SW_SHOW); SetForegroundWindow(g_wnd); return; }
    LoadTestTheme();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = inst;
        wc.lpszClassName = L"FringelessTestPattern"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(C_CHROME);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&wc); reg = true;
    }
    g_wnd = CreateWindowW(L"FringelessTestPattern", APP_NAME L" — Test Pattern",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1120, 840, nullptr, nullptr, inst, nullptr);
    HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (big) SendMessageW(g_wnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(g_wnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
    BOOL dark = g_themeDark;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int corner = 2; /* DWMWA_WINDOW_CORNER_PREFERENCE = ROUND */
    DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));
    ShowWindow(g_wnd, SW_SHOW); UpdateWindow(g_wnd);
}
