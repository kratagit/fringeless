#pragma once
// Fringeless - shared application state and module interfaces.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_2.h>

#include <atomic>
#include <string>
#include <vector>

#define APP_NAME L"Fringeless"

// ---------------------------------------------------------------------------
// Settings: written by the UI thread, read by the render thread
// ---------------------------------------------------------------------------

enum Channel { kRed, kGreen, kBlue };
enum Axis { kX, kY };

struct Settings {
    std::atomic<float> shift[3][2];             // [channel][axis], pixels, -1..1
    std::atomic<bool>  enabled{ true };
    std::atomic<bool>  bicubic{ true };         // Catmull-Rom instead of bilinear

    std::atomic<bool>  pauseInFullscreen{ true };
    std::wstring       neverPause;              // ';'-separated exe names; fixed after load
    std::wstring       display;                 // preferred display, e.g. "DISPLAY1"
    // On/off shortcut (UI thread only). shortcutKey 0 = no shortcut.
    UINT shortcutModifiers = MOD_WIN | MOD_SHIFT;
    UINT shortcutKey = 'F';
    bool shortcutChosen = false;                // false: try the defaults

    Settings();
    void ResetShifts();
};

extern Settings g_settings;

void LoadSettings();
void SaveSettings();
std::wstring SettingsFolder();
std::wstring SettingsPath();
bool StartsWithWindows();
void SetStartWithWindows(bool on);
void RefreshStartupEntry();


// ---------------------------------------------------------------------------
// Displays
// ---------------------------------------------------------------------------

struct Display {
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput1*  output  = nullptr;
    std::wstring   name;     // monitor name, e.g. "XG27ACDMS"
    std::wstring   device;   // e.g. "DISPLAY1"
    RECT           rect{};   // desktop coordinates
    bool           primary = false;
};

extern std::vector<Display> g_displays;
extern std::atomic<int>     g_target;   // index into g_displays

void EnumerateDisplays();
void ReleaseDisplays();
int  PickDefaultDisplay();
bool TargetValid();
// Re-enumerates DXGI: cached outputs go stale after mode changes and GPU resets.
IDXGIOutput1* FindOutput(const std::wstring& device, IDXGIAdapter1** adapter);

// ---------------------------------------------------------------------------
// Runtime state published by the render thread
// ---------------------------------------------------------------------------

enum class PauseReason {
    None, ExclusiveFullscreen, NoForeground, Shell, NotCovering, Maximized, HasCaption, NeverPauseList,
    BorderlessFullscreen,
};

struct Runtime {
    std::atomic<bool> running{ true };
    std::atomic<bool> paused{ false };          // auto-paused for a fullscreen app

    std::atomic<bool> redrawAll{ true };        // settings changed: redraw the whole overlay
    std::atomic<int>  retarget{ 0 };            // bumped when the target display changes
    std::atomic<long> captureHr{ 0 };           // last Desktop Duplication result
    std::atomic<int>  format{ 0 };              // DXGI format of the captured desktop
    std::atomic<PauseReason> reason{ PauseReason::None };
};

extern Runtime g_runtime;

// Foreground window's process image name, lower case ("" if unknown).
std::wstring ProcessNameOf(HWND hwnd);

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

// DWM correction engine (src/dwm_engine.cpp). The correction runs inside dwm.exe; the app
// injects the engine (one UAC prompt) when correction is first enabled and drives it live.
void DwmEngineSync();       // after a settings change / at startup: inject on off->on, push params
void DwmEngineShutdown();   // release handles on exit; leaves the engine loaded in dwm
void DwmEngineStop();       // fully unload the engine from dwm (one UAC prompt)
bool DwmEngineReady();      // true once the engine's parameter block is mapped

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,      // tray icon callback
    WM_APP_STATE = WM_APP + 2,     // render thread changed state: refresh tray and panel
    WM_APP_QUIT = WM_APP + 3,      // exit (used by test scripts)
};

extern HWND g_panel;               // main window; also owns the hotkey and the tray icon
extern std::wstring g_hotkeyName;  // on/off shortcut in use, empty if none is registered

// On/off shortcut (main.cpp). Modifiers are MOD_* flags; key 0 means no shortcut.
std::wstring ShortcutText(UINT modifiers, UINT key);
bool SetToggleShortcut(UINT modifiers, UINT key);   // registers and saves; false if taken
void SuspendToggleShortcut();                       // while the user records a new one
void ResumeToggleShortcut();

bool CreatePanel(HINSTANCE instance, int showCmd);
void ShowPanel();
void RefreshPanel();

void UpdateTray();
void ShowTrayMenu(HWND owner);
void OnHotkey(HWND hwnd, WPARAM id);
void OnTray(HWND hwnd, LPARAM lp);

// Called after any settings change: persists and asks the renderer for a full redraw.
void SettingsChanged();

// Tiny helpers
template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
inline std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.rfind(L'\\');
    return p == std::wstring::npos ? path : path.substr(p + 1);
}
