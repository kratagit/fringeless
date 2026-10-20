// Control panel: a Windows 11 style window drawn with Direct2D and DirectWrite.
//
// The UI is immediate-mode: every paint lays out the cards and widgets from the current
// settings and records hit rects, which the input handlers use until the next paint.
// Colours follow the system light/dark theme and accent colour; on Windows 11 the
// window uses the Mica backdrop. Two views share the window: the main controls and
// the settings page.

#include "app.h"
#include "resource.h"

#include <commctrl.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>

namespace {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

struct Theme {
    D2D1_COLOR_F background, card, cardStroke, text, text2, text3, warning;
    D2D1_COLOR_F control, controlHover, controlPressed, controlStroke, track, thumb;
    D2D1_COLOR_F accent, accentHover, onAccent;
    D2D1_COLOR_F flyout, flyoutStroke;
    COLORREF editText, editBack;
};

Theme T;
bool  g_dark = true;
bool  g_mica = false;
HBRUSH g_editBrush;

D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

bool SystemUsesDarkTheme() {
    DWORD light = 0, size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS
               ? light == 0
               : true;
}

// The accent shades Windows itself uses: a light shade on dark backgrounds, a dark one on light.
D2D1_COLOR_F SystemAccent(bool dark) {
    BYTE palette[32];
    DWORD size = sizeof(palette);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
        size >= 32) {
        const BYTE* c = palette + (dark ? 4 : 16);   // Light2 / Dark1
        return D2D1::ColorF(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
    }
    return dark ? Rgb(0x60CDFF) : Rgb(0x005FB8);
}

void LoadTheme() {
    g_dark = SystemUsesDarkTheme();
    T.accent = SystemAccent(g_dark);
    if (g_dark) {
        T.background = Rgb(0x202020);
        T.card = g_mica ? Rgb(0xFFFFFF, 0.05f) : Rgb(0x2B2B2B);
        T.cardStroke = Rgb(0x000000, 0.2f);
        T.text = Rgb(0xFFFFFF);
        T.text2 = Rgb(0xFFFFFF, 0.79f);
        T.text3 = Rgb(0xFFFFFF, 0.42f);
        T.warning = Rgb(0xFCE100);
        T.control = Rgb(0xFFFFFF, 0.06f);
        T.controlHover = Rgb(0xFFFFFF, 0.085f);
        T.controlPressed = Rgb(0xFFFFFF, 0.03f);
        T.controlStroke = Rgb(0xFFFFFF, 0.08f);
        T.track = Rgb(0xFFFFFF, 0.54f);
        T.thumb = Rgb(0x454545);
        T.onAccent = Rgb(0x000000);
        T.accentHover = Mix(T.accent, Rgb(0x000000), 0.1f);
        T.flyout = Rgb(0x2C2C2C);
        T.flyoutStroke = Rgb(0x000000, 0.4f);
        T.editText = RGB(255, 255, 255);
        T.editBack = RGB(0x1F, 0x1F, 0x1F);
    } else {
        T.background = Rgb(0xF3F3F3);
        T.card = g_mica ? Rgb(0xFFFFFF, 0.7f) : Rgb(0xFBFBFB);
        T.cardStroke = Rgb(0x000000, 0.06f);
        T.text = Rgb(0x000000, 0.9f);
        T.text2 = Rgb(0x000000, 0.61f);
        T.text3 = Rgb(0x000000, 0.36f);
        T.warning = Rgb(0x9D5D00);
        T.control = Rgb(0xFFFFFF, 0.7f);
        T.controlHover = Rgb(0xF9F9F9, 0.5f);
        T.controlPressed = Rgb(0xF9F9F9, 0.3f);
        T.controlStroke = Rgb(0x000000, 0.1f);
        T.track = Rgb(0x000000, 0.45f);
        T.thumb = Rgb(0xFFFFFF);
        T.onAccent = Rgb(0xFFFFFF);
        T.accentHover = Mix(T.accent, Rgb(0xFFFFFF), 0.1f);
        T.flyout = Rgb(0xF9F9F9);
        T.flyoutStroke = Rgb(0x000000, 0.1f);
        T.editText = RGB(0x1A, 0x1A, 0x1A);
        T.editBack = RGB(255, 255, 255);
    }
    if (g_editBrush) DeleteObject(g_editBrush);
    g_editBrush = CreateSolidBrush(T.editBack);
}

void ApplyWindowTheme(HWND hwnd) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

// ---------------------------------------------------------------------------
// Direct2D resources
// ---------------------------------------------------------------------------

ID2D1Factory*          g_d2d;
IDWriteFactory*        g_dw;
ID2D1HwndRenderTarget* g_rt;      // target being drawn: the panel's, or the popup's while it paints
ID2D1SolidColorBrush*  g_brush;
ID2D1HwndRenderTarget* g_panelRt;
ID2D1SolidColorBrush*  g_panelBrush;
IDWriteTextFormat *g_fTitle, *g_fSubtitle, *g_fHeading, *g_fBody, *g_fCaption, *g_fIcon;
std::wstring g_glyphSettings, g_glyphBack;
UINT g_dpi = 96;

const float kWidth = 880.0f;   // client width in DIPs
const float kMargin = 24.0f;
const float kGap = 16.0f;
float g_contentHeight = 520.0f;

bool FontExists(const wchar_t* family) {
    IDWriteFontCollection* fonts = nullptr;
    g_dw->GetSystemFontCollection(&fonts);
    UINT32 index;
    BOOL exists = FALSE;
    if (fonts) fonts->FindFamilyName(family, &index, &exists);
    SafeRelease(fonts);
    return exists;
}

IDWriteTextFormat* MakeFormat(const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                           size, L"en-us", &f);
    return f;
}

bool CreateDeviceIndependentResources() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw)))
        return false;
    std::wstring family = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    g_fTitle = MakeFormat(family, 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fSubtitle = MakeFormat(family, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fHeading = MakeFormat(family, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fBody = MakeFormat(family, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fCaption = MakeFormat(family, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    // Windows 11 / 10 icon fonts; plain symbols as a last resort.
    const wchar_t* iconFont = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                            : FontExists(L"Segoe MDL2 Assets")  ? L"Segoe MDL2 Assets"
                                                                : nullptr;
    g_fIcon = MakeFormat(iconFont ? iconFont : L"Segoe UI Symbol", 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
    g_glyphSettings = iconFont ? L"" : L"⚙";
    g_glyphBack = iconFont ? L"" : L"←";
    return g_fTitle && g_fSubtitle && g_fHeading && g_fBody && g_fCaption && g_fIcon;
}

bool CreateTarget(HWND hwnd, ID2D1HwndRenderTarget** rt, ID2D1SolidColorBrush** brush, bool transparent) {
    if (*rt) return true;
    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, transparent ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
        (float)g_dpi, (float)g_dpi);
    if (FAILED(g_d2d->CreateHwndRenderTarget(props,
                                             D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                                             rt)))
        return false;
    (*rt)->SetTextAntialiasMode(transparent ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return SUCCEEDED((*rt)->CreateSolidColorBrush(T.text, brush));
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

enum Id {
    kNone, kDisplay, kEnabled,
    kShift0, kShift5 = kShift0 + 5,
    kReset, kBilinear, kBicubic, kPause, kQuit,
    kSettings, kBack, kRecord, kStartup, kOpenFolder, kTestPattern, kEngine,
    kValue0 = 100,   // + slider index: the value text, click to type a value
};

enum class Kind { Button, Toggle, Slider, Segment, Dropdown, Value };
enum class View { Main, Settings };

struct Hit {
    int id;
    Kind kind;
    D2D1_RECT_F rect;   // for sliders: the track
    bool enabled;
};

std::vector<Hit> g_hits;
bool g_drawing = true;          // false while measuring
View g_view = View::Main;
int  g_hot = kNone, g_active = kNone;
int  g_focus = kNone;           // slider that takes arrow keys
bool g_focusVisible = false;    // keyboard used since the last click: show the focus ring
float g_dragX = 0, g_dragValue = 0;
int  g_wheelSlider = kNone, g_wheelRemainder = 0;
bool g_startup = false;

// Shortcut recorder
bool g_recording = false;
std::wstring g_shortcutNote;
bool g_shortcutNoteIsError = false;

const UINT32 kChannelColour[3] = { 0xFF5B5B, 0x3FD16B, 0x4C8DFF };
const wchar_t* const kChannelName[3] = { L"Red", L"Green", L"Blue" };
const float kStep = 0.001f;     // finest shift step, pixels

bool IsSlider(int id) { return id >= kShift0 && id <= kShift5; }
float SliderValue(int id) { return g_settings.shift[(id - kShift0) / 2][(id - kShift0) % 2]; }

void SetSliderValue(int id, float v) {
    v = std::round(std::fmin(std::fmax(v, -1.0f), 1.0f) / kStep) * kStep;
    g_settings.shift[(id - kShift0) / 2][(id - kShift0) % 2] = v;
    g_runtime.redrawAll = true;   // live preview while adjusting
}

std::wstring SliderText(int id) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%+.3f px", (double)SliderValue(id));
    return buf;
}

void AddHit(int id, Kind kind, D2D1_RECT_F r, bool enabled) {
    if (g_drawing) g_hits.push_back({ id, kind, r, enabled });
}

void Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    if (!g_drawing) return;
    g_brush->SetColor(c);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_brush);
}

void Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1.0f) {
    if (!g_drawing) return;
    g_brush->SetColor(c);
    D2D1_RECT_F in = { r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2 };
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), g_brush, width);
}

void Circle(float cx, float cy, float radius, D2D1_COLOR_F c) {
    if (!g_drawing) return;
    g_brush->SetColor(c);
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), g_brush);
}

void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
          DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    if (!g_drawing) return;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(valign);
    g_brush->SetColor(c);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

DWRITE_TEXT_METRICS Measure(const std::wstring& s, IDWriteTextFormat* f, float width) {
    DWRITE_TEXT_METRICS m = {};
    IDWriteTextLayout* layout = nullptr;
    if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 10000.0f, &layout))) {
        layout->GetMetrics(&m);
        layout->Release();
    }
    return m;
}

bool IsHot(int id) { return g_hot == id && (g_active == kNone || g_active == id); }

void Button(int id, D2D1_RECT_F r, const std::wstring& label, bool accent = false, bool enabled = true) {
    AddHit(id, Kind::Button, r, enabled);
    bool pressed = g_active == id && g_hot == id;
    D2D1_COLOR_F fill = accent ? (pressed || IsHot(id) ? T.accentHover : T.accent)
                               : (pressed ? T.controlPressed : IsHot(id) ? T.controlHover : T.control);
    if (!enabled) fill = T.control;
    Fill(r, 4.0f, fill);
    if (!accent) Stroke(r, 4.0f, T.controlStroke);
    Text(label, g_fBody, r, !enabled ? T.text3 : accent ? T.onAccent : T.text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void IconButton(int id, D2D1_RECT_F r, const std::wstring& glyph) {
    AddHit(id, Kind::Button, r, true);
    if (g_active == id && g_hot == id) Fill(r, 4.0f, T.controlPressed);
    else if (IsHot(id)) Fill(r, 4.0f, T.controlHover);
    Text(glyph, g_fIcon, r, T.text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Toggle(int id, float right, float cy, bool on, bool enabled) {
    D2D1_RECT_F r = { right - 40.0f, cy - 10.0f, right, cy + 10.0f };
    AddHit(id, Kind::Toggle, { r.left - 4, r.top - 6, r.right + 4, r.bottom + 6 }, enabled);
    float knob = IsHot(id) && enabled ? 7.0f : 6.0f;
    if (g_active == id && g_hot == id) knob = 5.0f;
    if (on) {
        Fill(r, 10.0f, enabled ? (IsHot(id) ? T.accentHover : T.accent) : T.text3);
        Circle(r.right - 10.0f, cy, knob, T.onAccent);
    } else {
        Fill(r, 10.0f, IsHot(id) && enabled ? T.controlHover : T.control);
        Stroke(r, 10.0f, enabled ? T.text2 : T.text3);
        Circle(r.left + 10.0f, cy, knob, enabled ? T.text2 : T.text3);
    }
}

void Chevron(float cx, float cy, D2D1_COLOR_F c) {
    if (!g_drawing) return;
    g_brush->SetColor(c);
    g_rt->DrawLine(D2D1::Point2F(cx - 4.5f, cy - 2.0f), D2D1::Point2F(cx, cy + 2.5f), g_brush, 1.3f);
    g_rt->DrawLine(D2D1::Point2F(cx, cy + 2.5f), D2D1::Point2F(cx + 4.5f, cy - 2.0f), g_brush, 1.3f);
}

// Row layouts. Each takes the content area of a card and returns the next y.

float ToggleRow(float x, float y, float w, int id, const std::wstring& label, const std::wstring& hint, bool on,
                bool enabled = true) {
    Text(label, g_fBody, { x, y, x + w, y + 40.0f }, enabled ? T.text : T.text3);
    if (!hint.empty()) Text(hint, g_fCaption, { x, y, x + w - 56.0f, y + 40.0f }, T.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Toggle(id, x + w, y + 20.0f, on, enabled);
    return y + 40.0f;
}

D2D1_RECT_F ValueRect(float x, float y, float w) { return { x + w - 82.0f, y + 5.0f, x + w, y + 31.0f }; }

float SliderRow(float x, float y, float w, int id, const std::wstring& label, int channel) {
    const float h = 36.0f, labelW = 92.0f;
    Circle(x + 5.0f, y + h / 2, 5.0f, Rgb(kChannelColour[channel]));
    Text(label, g_fBody, { x + 18.0f, y, x + labelW, y + h }, T.text);

    // The value doubles as a button that opens a text box for typing an exact value.
    int valueId = kValue0 + (id - kShift0);
    D2D1_RECT_F vr = ValueRect(x, y, w);
    AddHit(valueId, Kind::Value, vr, true);
    if (IsHot(valueId)) Fill(vr, 4.0f, T.controlHover);
    Text(SliderText(id), g_fBody, { vr.left, vr.top, vr.right - 6.0f, vr.bottom }, T.text2,
         DWRITE_TEXT_ALIGNMENT_TRAILING);

    D2D1_RECT_F track = { x + labelW + 10.0f, y + h / 2 - 2.0f, vr.left - 14.0f, y + h / 2 + 2.0f };
    AddHit(id, Kind::Slider, track, true);
    float tw = track.right - track.left;
    float thumb = track.left + (SliderValue(id) + 1.0f) / 2.0f * tw;
    float origin = track.left + tw / 2;
    if (g_focus == id && g_focusVisible) Stroke({ track.left - 14, y + 2, track.right + 14, y + h - 2 }, 6.0f, T.accent, 1.5f);
    Fill(track, 2.0f, T.track);
    Fill({ std::min(origin, thumb), track.top, std::max(origin, thumb), track.bottom }, 2.0f, T.accent);
    Fill({ origin - 0.5f, track.top - 3.0f, origin + 0.5f, track.bottom + 3.0f }, 0.0f, T.text3);
    float inner = g_active == id ? 5.0f : IsHot(id) ? 7.0f : 6.0f;
    Circle(thumb, y + h / 2, 10.0f, T.thumb);
    if (g_drawing) {
        g_brush->SetColor(T.controlStroke);
        g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y + h / 2), 10.0f, 10.0f), g_brush, 1.0f);
    }
    Circle(thumb, y + h / 2, inner, T.accent);
    return y + h;
}

float Caption(float x, float y, float w, const std::wstring& s, D2D1_COLOR_F c) {
    float h = Measure(s, g_fCaption, w).height;
    Text(s, g_fCaption, { x, y, x + w, y + h + 2.0f }, c, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return y + h;
}

// Card body: (x, y, width, bottom) -> content bottom. bottom is 0 while measuring and
// otherwise the final content bottom, so widgets can be anchored to the card's foot.
using Body = std::function<float(float, float, float, float)>;

// A card: background, heading, then the body below it. stretchTo extends the card down
// to that y so the two columns end on the same line.
void Card(float x, float& y, float w, const std::wstring& heading, const Body& body, float stretchTo = 0.0f) {
    const float pad = 16.0f, headH = 24.0f;
    float cx = x + pad, cy = y + pad + headH + 4.0f, cw = w - 2 * pad;
    bool drawing = g_drawing;
    g_drawing = false;
    float bottom = std::max(body(cx, cy, cw, 0.0f) + pad, stretchTo);
    g_drawing = drawing;
    D2D1_RECT_F r = { x, y, x + w, bottom };
    Fill(r, 8.0f, T.card);
    Stroke(r, 8.0f, T.cardStroke);
    Text(heading, g_fHeading, { cx, y + pad, x + w - pad, y + pad + headH }, T.text);
    body(cx, cy, cw, bottom - pad);
    y = bottom + kGap;
}

float AtFoot(float y, float bottom, float h) { return bottom > 0.0f ? std::max(y, bottom - h) : y; }

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

enum class State { Off, Paused, Waiting, On };

State CurrentState() {
    if (!g_settings.enabled) return State::Off;
    if (g_runtime.paused) return State::Paused;
    if (g_runtime.captureHr < 0) return State::Waiting;
    return State::On;
}

std::wstring StatusText() {
    wchar_t buf[256];
    std::wstring name = TargetValid() ? g_displays[g_target].name : L"the display";
    switch (CurrentState()) {
        case State::Off:
            return g_hotkeyName.empty() ? L"Correction is off. Turn it on here or from the tray icon."
                                        : L"Correction is off. Turn it on here, from the tray icon or with " +
                                              g_hotkeyName + L".";
        case State::Paused:
            return g_runtime.reason == PauseReason::ExclusiveFullscreen
                       ? L"Paused while a fullscreen game or video is in front. Resumes automatically."
                       : L"Paused while a borderless fullscreen app is in front. Resumes automatically.";
        case State::Waiting:
            swprintf(buf, 256, L"Waiting for %ls (0x%08lX). Retrying...", name.c_str(),
                     (unsigned long)g_runtime.captureHr);
            return buf;
        case State::On: break;
    }
    int fmt = g_runtime.format;
    const wchar_t* depth = fmt == 24 ? L"10-bit desktop" : fmt == 87 ? L"8-bit desktop" : L"desktop";
    swprintf(buf, 256, L"Correcting %ls (%ls).", name.c_str(), depth);
    return buf;
}

float Header(float x, float y, float w) {
    if (g_view == View::Settings) {
        IconButton(kBack, { x - 6.0f, y + 2.0f, x + 32.0f, y + 40.0f }, g_glyphBack);
        Text(L"Settings", g_fTitle, { x + 44.0f, y, x + w, y + 42.0f }, T.text);
        Text(APP_NAME, g_fSubtitle, { x + 44.0f, y + 38.0f, x + w, y + 58.0f }, T.text2);
        return y + 64.0f;
    }
    Text(APP_NAME, g_fTitle, { x, y, x + w, y + 38.0f }, T.text);
    Text(L"Subpixel colour-fringe correction for QD-OLED displays", g_fSubtitle, { x, y + 38.0f, x + w, y + 58.0f },
         T.text2);

    D2D1_RECT_F gear = { x + w - 36.0f, y + 4.0f, x + w, y + 40.0f };
    IconButton(kSettings, gear, g_glyphSettings);

    static const wchar_t* const labels[] = { L"Off", L"Paused", L"Waiting", L"On" };
    State st = CurrentState();
    D2D1_COLOR_F dot = st == State::On ? Rgb(0x3FD16B) : st == State::Off ? T.text3 : Rgb(0xF2B233);
    std::wstring label = labels[(int)st];
    float pillW = 38.0f + Measure(label, g_fBody, 1000.0f).widthIncludingTrailingWhitespace;
    D2D1_RECT_F pill = { gear.left - 8.0f - pillW, y + 8.0f, gear.left - 8.0f, y + 36.0f };
    Fill(pill, 14.0f, T.control);
    Stroke(pill, 14.0f, T.controlStroke);
    Circle(pill.left + 16.0f, (pill.top + pill.bottom) / 2, 4.0f, dot);
    Text(label, g_fBody, { pill.left + 26.0f, pill.top, pill.right - 10.0f, pill.bottom }, T.text);
    return y + 64.0f;
}

void DisplayDropdown(float x, float y, float w) {
    D2D1_RECT_F dd = { x + w - 260.0f, y + 4.0f, x + w, y + 36.0f };
    AddHit(kDisplay, Kind::Dropdown, dd, true);
    Fill(dd, 4.0f, IsHot(kDisplay) ? T.controlHover : T.control);
    Stroke(dd, 4.0f, T.controlStroke);
    std::wstring name = TargetValid() ? g_displays[g_target].name : L"(none)";
    Text(name, g_fBody, { dd.left + 12.0f, dd.top, dd.right - 30.0f, dd.bottom }, T.text);
    Chevron(dd.right - 16.0f, (dd.top + dd.bottom) / 2, T.text2);
}

float MainLeft(float x, float y, float w, float stretch) {
    Card(x, y, w, L"Display", [](float cx, float cy, float cw, float) {
        Text(L"Monitor", g_fBody, { cx, cy, cx + cw, cy + 40.0f }, T.text);
        DisplayDropdown(cx, cy, cw);
        cy = ToggleRow(cx, cy + 40.0f, cw, kEnabled, L"Correction", g_hotkeyName, g_settings.enabled);
        return Caption(cx, cy + 6.0f, cw, StatusText(), T.text2);
    });
    Card(x, y, w, L"Behaviour", [](float cx, float cy, float cw, float bottom) {
        cy = ToggleRow(cx, cy, cw, kPause, L"Pause in fullscreen apps", L"", g_settings.pauseInFullscreen);
        cy = Caption(cx, cy, cw,
                     L"Games and video players pause correction; browsers, editors and terminals in full screen "
                     L"keep it.",
                     T.text2) + 16.0f;

        bool engineLoaded = DwmEngineReady();
        Text(engineLoaded ? L"Correction engine: loaded" : L"Correction engine: not loaded", g_fBody,
             { cx, cy, cx + cw - 118.0f, cy + 32.0f }, T.text2);
        Button(kEngine, { cx + cw - 108.0f, cy, cx + cw, cy + 32.0f }, engineLoaded ? L"Eject" : L"Inject");
        cy += 44.0f;

        cy = AtFoot(cy, bottom, 32.0f);
        Button(kTestPattern, { cx, cy, cx + 130.0f, cy + 32.0f }, L"Test pattern");
        Button(kQuit, { cx + cw - 130.0f, cy, cx + cw, cy + 32.0f }, L"Quit " APP_NAME);
        return cy + 32.0f;
    }, stretch);
    return y;
}

float MainRight(float x, float y, float w, float stretch) {
    Card(x, y, w, L"Channel shift", [](float cx, float cy, float cw, float bottom) {
        cy = Caption(cx, cy, cw,
                     L"In pixels: +X moves a channel right, +Y moves it down. Scroll or use the arrow keys for "
                     L"0.001 px steps, hold Shift while dragging for fine control, or click a value to type it.",
                     T.text2) + 6.0f;
        for (int c = 0; c < 3; ++c)
            for (int a = 0; a < 2; ++a)
                cy = SliderRow(cx, cy, cw, kShift0 + c * 2 + a,
                               std::wstring(kChannelName[c]) + (a == kX ? L" X" : L" Y"), c);
        cy += 8.0f;

        Text(L"Interpolation", g_fBody, { cx, cy, cx + cw, cy + 40.0f }, T.text);
        D2D1_RECT_F seg = { cx + cw - 200.0f, cy + 4.0f, cx + cw, cy + 36.0f };
        Fill(seg, 4.0f, T.control);
        Stroke(seg, 4.0f, T.controlStroke);
        const int ids[2] = { kBilinear, kBicubic };
        const wchar_t* names[2] = { L"Bilinear", L"Bicubic" };
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F r = { seg.left + i * 100.0f + 3.0f, seg.top + 3.0f, seg.left + (i + 1) * 100.0f - 3.0f,
                              seg.bottom - 3.0f };
            bool selected = (i == 1) == g_settings.bicubic;
            AddHit(ids[i], Kind::Segment, r, true);
            if (selected) Fill(r, 3.0f, T.accent);
            else if (IsHot(ids[i])) Fill(r, 3.0f, T.controlHover);
            Text(names[i], g_fBody, r, selected ? T.onAccent : T.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        cy = Caption(cx, cy + 40.0f, cw,
                     L"Bicubic keeps text crisper at larger shifts; bilinear is slightly softer.", T.text2) + 16.0f;
        cy = AtFoot(cy, bottom, 32.0f);
        Button(kReset, { cx + cw - 130.0f, cy, cx + cw, cy + 32.0f }, L"Reset shifts");
        return cy + 32.0f;
    }, stretch);
    return y;
}

float SettingsLeft(float x, float y, float w, float stretch) {
    Card(x, y, w, L"Shortcut", [](float cx, float cy, float cw, float) {
        Text(L"Turn correction on or off", g_fBody, { cx, cy, cx + cw, cy + 40.0f }, T.text);
        D2D1_RECT_F b = { cx + cw - 190.0f, cy + 4.0f, cx + cw, cy + 36.0f };
        const Settings& s = g_settings;
        std::wstring label = g_recording ? L"Press a shortcut..."
                           : s.shortcutKey ? ShortcutText(s.shortcutModifiers, s.shortcutKey)
                                           : L"Not set";
        Button(kRecord, b, label, g_recording);
        cy += 46.0f;
        std::wstring note = g_recording
            ? L"Press the keys together. Esc cancels, Backspace removes the shortcut."
            : !g_shortcutNote.empty() ? g_shortcutNote
            : s.shortcutKey && g_hotkeyName.empty()
                ? ShortcutText(s.shortcutModifiers, s.shortcutKey) + L" is used by another app. Click to pick another one."
                : L"Click the button and press a new combination, e.g. Win+Shift+F. Combinations with the Windows "
                  L"key never get in the way of typing.";
        bool error = !g_recording && (g_shortcutNoteIsError || (s.shortcutKey && g_hotkeyName.empty()));
        return Caption(cx, cy, cw, note, error ? T.warning : T.text2);
    });
    Card(x, y, w, L"Startup", [](float cx, float cy, float cw, float) {
        cy = ToggleRow(cx, cy, cw, kStartup, L"Start with Windows", L"", g_startup);
        return Caption(cx, cy, cw, APP_NAME L" starts in the tray when you sign in.", T.text2);
    }, stretch);
    return y;
}

float SettingsRight(float x, float y, float w, float stretch) {
    Card(x, y, w, L"Settings file", [](float cx, float cy, float cw, float bottom) {
        cy = Caption(cx, cy, cw, L"Your settings are saved automatically to:", T.text2) + 6.0f;
        cy = Caption(cx, cy, cw, SettingsPath(), T.text) + 16.0f;
        cy = AtFoot(cy, bottom, 32.0f);
        Button(kOpenFolder, { cx + cw - 130.0f, cy, cx + cw, cy + 32.0f }, L"Open folder");
        return cy + 32.0f;
    }, stretch);
    return y;
}

using Column = float (*)(float, float, float, float);

// Two columns whose last cards are stretched to end on the same line.
float Columns(float y, Column left, Column right) {
    float colW = (kWidth - 2 * kMargin - kGap) / 2;
    float lx = kMargin, rx = kMargin + colW + kGap;
    bool drawing = g_drawing;
    g_drawing = false;
    float lb = left(lx, y, colW, 0.0f) - kGap;
    float rb = right(rx, y, colW, 0.0f) - kGap;
    g_drawing = drawing;
    // When painting, also fill the window: both views share the larger view's height.
    float bottom = std::max({ lb, rb, drawing ? g_contentHeight - kMargin : 0.0f });
    left(lx, y, colW, bottom);
    right(rx, y, colW, bottom);
    return bottom + kMargin;
}

// Lays out (and, when g_drawing, paints) the current view. Returns the content height.
float Layout() {
    g_hits.clear();
    float y = Header(kMargin, kMargin - 4.0f, kWidth - 2 * kMargin) + 4.0f;
    return g_view == View::Main ? Columns(y, MainLeft, MainRight) : Columns(y, SettingsLeft, SettingsRight);
}

void PaintPanel(HWND hwnd) {
    if (!CreateTarget(hwnd, &g_panelRt, &g_panelBrush, g_mica)) return;
    g_rt = g_panelRt;
    g_brush = g_panelBrush;
    g_rt->BeginDraw();
    g_rt->Clear(g_mica ? D2D1::ColorF(0, 0, 0, 0) : T.background);
    g_drawing = true;
    Layout();
    if (g_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        SafeRelease(g_panelBrush);
        SafeRelease(g_panelRt);
    }
}

// ---------------------------------------------------------------------------
// Typing an exact value
// ---------------------------------------------------------------------------

HWND  g_edit;
int   g_editSlider = kNone;
HFONT g_editFont;
const UINT WM_APP_END_EDIT = WM_APP + 20;   // wParam: apply

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wp == VK_RETURN || wp == VK_ESCAPE) {
                PostMessageW(g_panel, WM_APP_END_EDIT, wp == VK_RETURN, 0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == '\r' || wp == 27) return 0;   // no beep
            break;
        case WM_KILLFOCUS:
            PostMessageW(g_panel, WM_APP_END_EDIT, 1, 0);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void EndEdit(bool apply) {
    if (!g_edit) return;
    HWND edit = g_edit;
    g_edit = nullptr;
    if (apply) {
        wchar_t buf[64] = {};
        GetWindowTextW(edit, buf, 64);
        for (wchar_t& c : buf)
            if (c == L',') c = L'.';
        wchar_t* end = nullptr;
        double v = wcstod(buf, &end);
        if (end != buf) {
            SetSliderValue(g_editSlider, (float)v);
            SettingsChanged();
        }
    }
    DestroyWindow(edit);
    InvalidateRect(g_panel, nullptr, FALSE);
}

void BeginEdit(HWND hwnd, int slider, D2D1_RECT_F r) {
    EndEdit(true);
    float s = g_dpi / 96.0f;
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-(int)std::lround(14.0f * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int h = (int)std::lround(22.0f * s);
    int top = (int)std::lround((r.top + r.bottom) / 2 * s) - h / 2;
    g_editSlider = slider;
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
                             (int)std::lround(r.left * s), top, (int)std::lround((r.right - r.left) * s), h, hwnd,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    wchar_t buf[32];
    swprintf(buf, 32, L"%.3f", (double)SliderValue(slider));
    SetWindowTextW(g_edit, buf);
    SetWindowSubclass(g_edit, EditProc, 1, 0);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
}

// ---------------------------------------------------------------------------
// Recording the on/off shortcut
// ---------------------------------------------------------------------------

void StartRecording(HWND hwnd) {
    g_recording = true;
    g_shortcutNote.clear();
    g_shortcutNoteIsError = false;
    SuspendToggleShortcut();   // so pressing the current one records it instead of toggling
    SetFocus(hwnd);
}

void StopRecording(bool restore) {
    if (!g_recording) return;
    g_recording = false;
    if (restore) ResumeToggleShortcut();
}

UINT HeldModifiers() {
    UINT m = 0;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    if (GetKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    return m;
}

void RecordKey(UINT vk) {
    switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
            return;   // wait for the actual key
    }
    UINT mods = HeldModifiers();
    auto reject = [](const std::wstring& why) {
        g_shortcutNote = why;
        g_shortcutNoteIsError = true;
    };
    if (!mods && vk == VK_ESCAPE) {
        StopRecording(true);
        return;
    }
    if (!mods && (vk == VK_BACK || vk == VK_DELETE)) {
        StopRecording(false);
        SetToggleShortcut(0, 0);
        g_shortcutNote = L"No shortcut. Correction can still be switched here and from the tray icon.";
        g_shortcutNoteIsError = false;
        return;
    }
    bool functionKey = vk >= VK_F1 && vk <= VK_F24;
    if (!(mods & (MOD_WIN | MOD_CONTROL | MOD_ALT)) && !functionKey) {
        reject(L"Use the Windows key, Ctrl or Alt together with the key, or a function key.");
        return;
    }
    // Ctrl+Alt is AltGr on many layouts: Ctrl+Alt+<character key> would block typing.
    if ((mods & MOD_CONTROL) && (mods & MOD_ALT) && !(mods & MOD_WIN) && MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR)) {
        reject(L"Ctrl+Alt with a character key works like AltGr and would block typing letters. Add the Windows "
               L"key or pick a different key.");
        return;
    }
    StopRecording(false);
    std::wstring text = ShortcutText(mods, vk);
    if (SetToggleShortcut(mods, vk)) {
        g_shortcutNote = L"Saved. " + text + L" now turns correction on and off.";
        g_shortcutNoteIsError = false;
    } else {
        g_shortcutNote = text + L" is already used by another app. The previous shortcut was kept.";
        g_shortcutNoteIsError = true;
    }
    UpdateTray();
}

// ---------------------------------------------------------------------------
// Display list (a Windows 11 style flyout)
// ---------------------------------------------------------------------------

HWND g_popup;
ID2D1HwndRenderTarget* g_popupRt;
ID2D1SolidColorBrush* g_popupBrush;
int g_popupHot = -1;
ULONGLONG g_popupClosedAt;
const float kItemH = 44.0f, kPopupPad = 4.0f, kPopupW = 340.0f;

void ChooseDisplay(int index) {
    if (index < 0 || index >= (int)g_displays.size() || index == g_target) return;
    g_target = index;
    g_settings.display = g_displays[index].device;
    g_runtime.retarget++;
    SettingsChanged();
    InvalidateRect(g_panel, nullptr, FALSE);
}

void PaintPopup(HWND hwnd) {
    if (!CreateTarget(hwnd, &g_popupRt, &g_popupBrush, false)) return;
    ID2D1HwndRenderTarget* rt = g_rt;
    ID2D1SolidColorBrush* brush = g_brush;
    g_rt = g_popupRt;
    g_brush = g_popupBrush;
    g_drawing = true;
    g_rt->BeginDraw();
    g_rt->Clear(T.flyout);
    for (size_t i = 0; i < g_displays.size(); ++i) {
        const Display& d = g_displays[i];
        D2D1_RECT_F r = { kPopupPad, kPopupPad + i * kItemH, kPopupW - kPopupPad, kPopupPad + (i + 1) * kItemH };
        bool selected = (int)i == g_target;
        if ((int)i == g_popupHot) Fill(r, 4.0f, T.controlHover);
        else if (selected) Fill(r, 4.0f, T.control);
        if (selected) Fill({ r.left + 2.0f, r.top + 14.0f, r.left + 5.0f, r.bottom - 14.0f }, 1.5f, T.accent);
        Text(d.name, g_fBody, { r.left + 16.0f, r.top, r.right - 130.0f, r.bottom }, T.text);
        wchar_t info[64];
        swprintf(info, 64, L"%ld × %ld%ls", d.rect.right - d.rect.left, d.rect.bottom - d.rect.top,
                 d.primary ? L"  ·  primary" : L"");
        Text(info, g_fCaption, { r.right - 170.0f, r.top, r.right - 12.0f, r.bottom }, T.text3,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (g_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        SafeRelease(g_popupBrush);
        SafeRelease(g_popupRt);
    }
    g_rt = rt;
    g_brush = brush;
}

int PopupItemAt(LPARAM lp) {
    float y = GET_Y_LPARAM(lp) * 96.0f / g_dpi - kPopupPad;
    int i = y < 0 ? -1 : (int)(y / kItemH);
    return i >= 0 && i < (int)g_displays.size() ? i : -1;
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            PaintPopup(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            int hot = PopupItemAt(lp);
            if (hot != g_popupHot) {
                g_popupHot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_popupHot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            int i = PopupItemAt(lp);
            if (i < 0) return 0;
            DestroyWindow(hwnd);
            ChooseDisplay(i);
            return 0;
        }
        case WM_KEYDOWN: {
            int n = (int)g_displays.size();
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            else if (wp == VK_UP || wp == VK_DOWN) {
                int start = g_popupHot >= 0 ? g_popupHot : g_target.load();
                g_popupHot = ((start + (wp == VK_DOWN ? 1 : -1)) % n + n) % n;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == VK_RETURN && g_popupHot >= 0) {
                int i = g_popupHot;
                DestroyWindow(hwnd);
                ChooseDisplay(i);
            }
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            SafeRelease(g_popupBrush);
            SafeRelease(g_popupRt);
            g_popup = nullptr;
            g_popupClosedAt = GetTickCount64();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenDisplayPopup(HWND owner, const D2D1_RECT_F& anchor) {
    if (g_popup) return;
    float s = g_dpi / 96.0f;
    int w = (int)std::lround(kPopupW * s);
    int h = (int)std::lround((2 * kPopupPad + kItemH * g_displays.size()) * s);
    POINT pt = { (LONG)std::lround(anchor.right * s) - w, (LONG)std::lround((anchor.bottom + 4.0f) * s) };
    ClientToScreen(owner, &pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    if (pt.y + h > mi.rcWork.bottom) {   // no room below: open above the button
        POINT top = { 0, (LONG)std::lround((anchor.top - 4.0f) * s) };
        ClientToScreen(owner, &top);
        pt.y = top.y - h;
    }
    g_popupHot = -1;
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"FringelessFlyout", L"", WS_POPUP, pt.x, pt.y, w, h, owner, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!g_popup) return;
    int corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(g_popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    ApplyWindowTheme(g_popup);
    ShowWindow(g_popup, SW_SHOW);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

D2D1_POINT_2F ToDips(LPARAM lp) {
    return D2D1::Point2F(GET_X_LPARAM(lp) * 96.0f / g_dpi, GET_Y_LPARAM(lp) * 96.0f / g_dpi);
}

const Hit* HitAt(D2D1_POINT_2F p) {
    for (const Hit& h : g_hits) {
        D2D1_RECT_F r = h.rect;
        if (h.kind == Kind::Slider) {   // generous target around the thin track
            r.top -= 14.0f;
            r.bottom += 14.0f;
            r.left -= 10.0f;
            r.right += 10.0f;
        }
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return &h;
    }
    return nullptr;
}

const Hit* FindHit(int id) {
    for (const Hit& h : g_hits)
        if (h.id == id) return &h;
    return nullptr;
}

float TrackValueAt(const Hit& h, float x) {
    float t = (x - h.rect.left) / (h.rect.right - h.rect.left);
    return -1.0f + std::fmin(std::fmax(t, 0.0f), 1.0f) * 2.0f;
}

void SwitchView(View v) {
    EndEdit(true);
    StopRecording(true);
    g_view = v;
    g_focus = kNone;
    g_shortcutNote.clear();
    g_startup = StartsWithWindows();
}

void Activate(HWND hwnd, const Hit& h) {
    Settings& s = g_settings;
    switch (h.id) {
        case kEnabled:  s.enabled = !s.enabled; break;
        case kPause:    s.pauseInFullscreen = !s.pauseInFullscreen; break;
        case kBilinear: s.bicubic = false; break;
        case kBicubic:  s.bicubic = true; break;
        case kReset:    s.ResetShifts(); break;
        case kQuit:     DestroyWindow(hwnd); return;
        case kSettings: SwitchView(View::Settings); return;
        case kBack:     SwitchView(View::Main); return;
        case kRecord:
            if (g_recording) StopRecording(true);
            else StartRecording(hwnd);
            return;
        case kStartup:
            SetStartWithWindows(!g_startup);
            g_startup = StartsWithWindows();
            return;
        case kOpenFolder:
            ShellExecuteW(hwnd, L"open", SettingsFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case kTestPattern:
            OpenTestPattern();
            return;
        case kEngine:
            if (DwmEngineReady()) DwmEngineStop(); else DwmEngineInject();
            UpdateTray();
            RefreshPanel();
            return;
        default: return;
    }
    SettingsChanged();
}

void ResizeToContent(HWND hwnd, const RECT* suggested) {
    RECT rc = { 0, 0, (LONG)std::ceil(kWidth * g_dpi / 96.0f), (LONG)std::ceil(g_contentHeight * g_dpi / 96.0f) };
    typedef BOOL(WINAPI * AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static auto adjust = (AdjustForDpiFn)(void*)GetProcAddress(GetModuleHandleW(L"user32"), "AdjustWindowRectExForDpi");
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (adjust) adjust(&rc, style, FALSE, ex, g_dpi);
    else AdjustWindowRectEx(&rc, style, FALSE, ex);
    int x = suggested ? suggested->left : 0, y = suggested ? suggested->top : 0;
    SetWindowPos(hwnd, nullptr, x, y, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));
}

// Both views get the same window size, the larger of the two.
void MeasureViews() {
    View current = g_view;
    g_drawing = false;
    g_contentHeight = 0.0f;
    g_view = View::Main;
    float main = Layout();
    g_view = View::Settings;
    float settings = Layout();
    g_view = current;
    g_drawing = true;
    g_contentHeight = std::max(main, settings);
}

void Repaint(HWND hwnd) { InvalidateRect(hwnd, nullptr, FALSE); }

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            PaintPanel(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (g_panelRt) g_panelRt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            Repaint(hwnd);
            return 0;
        case WM_DPICHANGED:
            EndEdit(true);
            g_dpi = HIWORD(wp);
            if (g_panelRt) g_panelRt->SetDpi((float)g_dpi, (float)g_dpi);
            ResizeToContent(hwnd, (const RECT*)lp);
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) {
                LoadTheme();
                ApplyWindowTheme(hwnd);
                Repaint(hwnd);
            }
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wp, T.editText);
            SetBkColor((HDC)wp, T.editBack);
            return (LRESULT)g_editBrush;
        case WM_APP_END_EDIT:
            EndEdit(wp != 0);
            return 0;
        case WM_TIMER: {
            // Repaint only when the status line would actually change.
            static std::wstring shown;
            std::wstring now = std::to_wstring((int)CurrentState()) + StatusText();
            if (now != shown) {
                shown = now;
                Repaint(hwnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            D2D1_POINT_2F p = ToDips(lp);
            if (g_active != kNone && IsSlider(g_active)) {
                const Hit* h = FindHit(g_active);
                if (h) {
                    // Relative drag; Shift slows it down tenfold for fine adjustment.
                    float perDip = 2.0f / (h->rect.right - h->rect.left);
                    g_dragValue += (p.x - g_dragX) * perDip * ((wp & MK_SHIFT) ? 0.1f : 1.0f);
                    g_dragValue = std::fmin(std::fmax(g_dragValue, -1.0f), 1.0f);
                    g_dragX = p.x;
                    SetSliderValue(g_active, g_dragValue);
                }
            }
            const Hit* h = HitAt(p);
            int hot = h && h->enabled ? h->id : kNone;
            if (hot != g_hot || g_active != kNone) {
                g_hot = hot;
                Repaint(hwnd);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_active == kNone && g_hot != kNone) {
                g_hot = kNone;
                Repaint(hwnd);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetFocus(hwnd);   // also commits an open value editor
            D2D1_POINT_2F p = ToDips(lp);
            const Hit* h = HitAt(p);
            g_focusVisible = false;
            if (!h || !h->enabled) {
                g_focus = kNone;
                Repaint(hwnd);
                return 0;
            }
            if (h->kind == Kind::Slider) {
                g_focus = h->id;
                if (msg == WM_LBUTTONDBLCLK) {
                    SetSliderValue(h->id, 0.0f);
                    SettingsChanged();
                    Repaint(hwnd);
                    return 0;
                }
                g_active = h->id;
                g_dragX = p.x;
                // A plain click jumps to the pointer; with Shift the value stays put.
                g_dragValue = (wp & MK_SHIFT) ? SliderValue(h->id) : TrackValueAt(*h, p.x);
                SetSliderValue(h->id, g_dragValue);
                SetCapture(hwnd);
            } else if (h->kind == Kind::Value) {
                g_focus = kShift0 + (h->id - kValue0);
                BeginEdit(hwnd, g_focus, h->rect);
            } else if (h->kind == Kind::Dropdown) {
                if (GetTickCount64() - g_popupClosedAt > 250) OpenDisplayPopup(hwnd, h->rect);
            } else {
                g_active = h->id;
                SetCapture(hwnd);
            }
            Repaint(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            int active = g_active;
            g_active = kNone;
            ReleaseCapture();
            const Hit* a = FindHit(active);
            const Hit* h = HitAt(ToDips(lp));
            if (a && a->kind == Kind::Slider) {
                SettingsChanged();
            } else if (a && h && h->id == active) {
                Hit copy = *a;
                Activate(hwnd, copy);
            }
            Repaint(hwnd);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            const Hit* h = HitAt(D2D1::Point2F(pt.x * 96.0f / g_dpi, pt.y * 96.0f / g_dpi));
            if (!h || !IsSlider(h->id)) return 0;
            // One notch = 0.001 px (Ctrl: 0.01 px); partial notches from precision
            // touchpads accumulate.
            if (h->id != g_wheelSlider) {
                g_wheelSlider = h->id;
                g_wheelRemainder = 0;
            }
            g_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
            int notches = g_wheelRemainder / WHEEL_DELTA;
            g_wheelRemainder -= notches * WHEEL_DELTA;
            if (!notches) return 0;
            g_focus = h->id;
            SetSliderValue(h->id, SliderValue(h->id) + notches * kStep * ((wp & MK_CONTROL) ? 10.0f : 1.0f));
            SettingsChanged();
            Repaint(hwnd);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_recording) {
                RecordKey((UINT)wp);
                Repaint(hwnd);
                return 0;
            }
            if (IsSlider(g_focus) && msg == WM_KEYDOWN) {
                float dir = (wp == VK_RIGHT || wp == VK_UP) ? 1.0f : (wp == VK_LEFT || wp == VK_DOWN) ? -1.0f : 0.0f;
                if (dir != 0.0f) {
                    float step = kStep * (GetKeyState(VK_SHIFT) < 0 ? 10.0f : 1.0f);
                    SetSliderValue(g_focus, SliderValue(g_focus) + dir * step);
                    SettingsChanged();
                    g_focusVisible = true;
                    Repaint(hwnd);
                    return 0;
                }
                if (wp == VK_ESCAPE) {
                    g_focus = kNone;
                    Repaint(hwnd);
                    return 0;
                }
            }
            break;
        case WM_SYSCHAR:
            if (g_recording) return 0;   // no menu beep while recording Alt combinations
            break;
        case WM_KILLFOCUS:
            if (g_recording && (HWND)wp != hwnd) {
                StopRecording(true);
                Repaint(hwnd);
            }
            return 0;

        case WM_HOTKEY:
            OnHotkey(hwnd, wp);
            return 0;
        case WM_APP_TRAY:
            OnTray(hwnd, lp);
            return 0;
        case WM_APP_STATE:
            UpdateTray();
            Repaint(hwnd);
            return 0;
        case WM_CLOSE:
            EndEdit(true);
            StopRecording(true);
            ShowWindow(hwnd, SW_HIDE);   // keeps running in the tray
            return 0;
        case WM_APP_QUIT:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_runtime.running = false;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}   // namespace

bool CreatePanel(HINSTANCE instance, int showCmd) {
    if (!CreateDeviceIndependentResources()) return false;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FringelessPanel";
    RegisterClassExW(&wc);

    WNDCLASSEXW fc = { sizeof(fc) };
    fc.style = CS_DROPSHADOW;
    fc.lpfnWndProc = PopupProc;
    fc.hInstance = instance;
    fc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    fc.lpszClassName = L"FringelessFlyout";
    RegisterClassExW(&fc);

    g_panel = CreateWindowExW(0, L"FringelessPanel", APP_NAME,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT,
                              CW_USEDEFAULT, 900, 600, nullptr, nullptr, instance, nullptr);
    if (!g_panel) return false;

    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    if (auto getDpi = (GetDpiForWindowFn)(void*)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow"))
        g_dpi = getDpi(g_panel);

    // Mica on Windows 11 22H2+: the backdrop shows through the transparent client area.
    int backdrop = 2;   // DWMSBT_MAINWINDOW
    g_mica = SUCCEEDED(DwmSetWindowAttribute(g_panel, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop)));
    if (g_mica) {
        MARGINS m = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_panel, &m);
    }
    LoadTheme();
    ApplyWindowTheme(g_panel);
    g_startup = StartsWithWindows();

    MeasureViews();
    ResizeToContent(g_panel, nullptr);
    SetTimer(g_panel, 1, 1000, nullptr);
    ShowWindow(g_panel, showCmd);
    if (showCmd != SW_HIDE) UpdateWindow(g_panel);
    return true;
}

void ShowPanel() {
    ShowWindow(g_panel, IsIconic(g_panel) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_panel);
}

void RefreshPanel() {
    if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
}
