// Fringeless - subpixel colour-fringe correction for QD-OLED displays.
//
// Startup, the on/off shortcut and the tray icon. The control panel lives in panel.cpp,
// the DWM correction engine in src/dwm_engine.cpp.

#include "app.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cwchar>

HWND g_panel;
static NOTIFYICONDATAW g_tray;

// Published runtime state (was the render thread's; now mostly reflects the on/off state).
Runtime g_runtime;

enum { HK_TOGGLE = 1 };
enum { IDM_OPEN = 100, IDM_TOGGLE, IDM_TEST, IDM_INJECT, IDM_EJECT, IDM_QUIT };

void SettingsChanged() {
    SaveSettings();
    g_runtime.redrawAll = true;
    DwmEngineSync();   // inject on an off->on edge, then publish the new parameters live
    UpdateTray();
}

void UpdateTray() {
    const wchar_t* name = TargetValid() ? g_displays[g_target].name.c_str() : L"no display";
    const wchar_t* state = !g_settings.enabled ? L"off"
                         : g_runtime.paused   ? L"paused for a fullscreen app"
                         : g_runtime.captureHr < 0 ? L"waiting for the display"
                                                   : L"correcting";
    swprintf(g_tray.szTip, 128, APP_NAME L" - %ls - %ls", state, name);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void ShowTrayMenu(HWND owner) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_OPEN, L"Open " APP_NAME);
    std::wstring toggle = L"Correction" + (g_hotkeyName.empty() ? L"" : L"\t" + g_hotkeyName);
    AppendMenuW(menu, MF_STRING | (g_settings.enabled ? MF_CHECKED : 0), IDM_TOGGLE, toggle.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TEST, L"Test pattern");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    bool loaded = DwmEngineReady();
    AppendMenuW(menu, MF_STRING | (loaded ? MF_GRAYED : 0), IDM_INJECT, L"Inject engine");
    AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), IDM_EJECT, L"Eject engine");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
    SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case IDM_OPEN: ShowPanel(); break;
        case IDM_TOGGLE:
            g_settings.enabled = !g_settings.enabled;
            SettingsChanged();
            RefreshPanel();
            break;
        case IDM_TEST: OpenTestPattern(); break;
        case IDM_INJECT: DwmEngineInject(); UpdateTray(); RefreshPanel(); break;
        case IDM_EJECT: DwmEngineStop(); UpdateTray(); RefreshPanel(); break;
        case IDM_QUIT: DestroyWindow(owner); break;
    }
}

// The only global shortcut turns correction on and off. By default it is Win+Shift+F,
// or Win+Ctrl+Alt+F when another app owns that; the user can record any other one.
// Combinations with the Windows key never collide with AltGr (= Ctrl+Alt).
std::wstring g_hotkeyName;

static std::wstring KeyName(UINT vk) {
    switch (vk) {
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_SPACE: return L"Space";
        case VK_PAUSE: return L"Pause";
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(scan << 16);
    switch (vk) {   // keys on the extended part of the keyboard
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_DIVIDE: case VK_NUMLOCK: case VK_SNAPSHOT:
            lp |= 1 << 24;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lp, name, 64) > 0) return name;
    wchar_t buf[16];
    swprintf(buf, 16, L"Key %u", vk);
    return buf;
}

std::wstring ShortcutText(UINT modifiers, UINT key) {
    if (!key) return L"";
    std::wstring s;
    if (modifiers & MOD_WIN) s += L"Win+";
    if (modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (modifiers & MOD_ALT) s += L"Alt+";
    if (modifiers & MOD_SHIFT) s += L"Shift+";
    return s + KeyName(key);
}

static bool Register(UINT modifiers, UINT key) {
    UnregisterHotKey(g_panel, HK_TOGGLE);
    g_hotkeyName.clear();
    if (!key) return true;
    if (!RegisterHotKey(g_panel, HK_TOGGLE, modifiers | MOD_NOREPEAT, key)) return false;
    g_hotkeyName = ShortcutText(modifiers, key);
    return true;
}

static void InitToggleShortcut() {
    Settings& s = g_settings;
    if (s.shortcutChosen) {
        Register(s.shortcutModifiers, s.shortcutKey);
        return;
    }
    const UINT defaults[] = { MOD_WIN | MOD_SHIFT, MOD_WIN | MOD_CONTROL | MOD_ALT };
    for (UINT mods : defaults)
        if (Register(mods, 'F')) {
            s.shortcutModifiers = mods;
            s.shortcutKey = 'F';
            return;
        }
}

bool SetToggleShortcut(UINT modifiers, UINT key) {
    Settings& s = g_settings;
    if (!Register(modifiers, key)) {
        Register(s.shortcutModifiers, s.shortcutKey);   // keep the previous one
        return false;
    }
    s.shortcutModifiers = modifiers;
    s.shortcutKey = key;
    s.shortcutChosen = true;
    SaveSettings();
    return true;
}

void SuspendToggleShortcut() {
    UnregisterHotKey(g_panel, HK_TOGGLE);
}

void ResumeToggleShortcut() {
    if (!Register(g_settings.shortcutModifiers, g_settings.shortcutKey)) g_hotkeyName.clear();
}
// Handles WM_HOTKEY for the panel window.
void OnHotkey(HWND, WPARAM id) {
    if (id != HK_TOGGLE) return;
    g_settings.enabled = !g_settings.enabled;
    SettingsChanged();
    RefreshPanel();
}

// Handles the tray callback for the panel window.
void OnTray(HWND hwnd, LPARAM lp) {
    switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            if (IsWindowVisible(hwnd) && GetForegroundWindow() == hwnd) ShowWindow(hwnd, SW_HIDE);
            else ShowPanel();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hwnd);
            break;
    }
}

static void AddTrayIcon(HINSTANCE inst) {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_panel;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = WM_APP_TRAY;
    g_tray.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    UpdateTray();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int showCmd) {
    // Launched elevated to inject/eject the engine into dwm.exe: do that and exit.
    int injMode = InjectModeMain(cmdLine);
    if (injMode >= 0) return injMode;

    // One instance: a second start just brings the running one forward.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Fringeless.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"FringelessPanel", nullptr)) {
            ShowWindow(other, SW_SHOW);
            SetForegroundWindow(other);
        }
        return 0;
    }

    typedef BOOL(WINAPI * SetDpiContextFn)(DPI_AWARENESS_CONTEXT);
    if (auto setDpi = (SetDpiContextFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                            "SetProcessDpiAwarenessContext"))
        setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    LoadSettings();
    EnumerateDisplays();
    if (g_displays.empty()) {
        MessageBoxW(nullptr, L"No displays were found.", APP_NAME, MB_ICONERROR);
        return 1;
    }
    g_target = PickDefaultDisplay();
    g_settings.display = g_displays[g_target].device;

    // Started with Windows: stay in the tray.
    if (cmdLine && wcsstr(cmdLine, L"--tray")) showCmd = SW_HIDE;
    RefreshStartupEntry();

    if (!CreatePanel(inst, showCmd)) return 1;
    InitToggleShortcut();
    RefreshPanel();
    AddTrayIcon(inst);

    // Correction runs inside dwm.exe via the DWM engine. Bring it up now if correction is
    // already enabled (prompts for UAC once); later toggles go through SettingsChanged.
    DwmEngineSync();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_runtime.running = false;
    DwmEngineStop();   // fully unload the engine from dwm when the app closes
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    SaveSettings();
    ReleaseDisplays();
    if (mutex) CloseHandle(mutex);
    return 0;
}
