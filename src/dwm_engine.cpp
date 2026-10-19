// Fringeless DWM engine control (app side).
//
// Drives the injected DWM correction DLL (src/dwm) from the app's settings. The DLL is
// injected into dwm.exe by the bundled, elevated helper fringeless_inject.exe; correction
// parameters are then published through a shared, memory-mapped file that this process
// writes on every settings change. See src/dwm for the engine itself (GNU GPL v3).

#include "app.h"

#include <shellapi.h>
#include <string>

#pragma pack(push, 4)
struct FringeParams {
    unsigned int magic;
    int          enabled;
    float        sx[3];
    float        sy[3];
    int          bicubic;
};
#pragma pack(pop)
static const unsigned int kFringeMagic = 0x46524E47u; // 'FRNG'

static volatile FringeParams* g_params = nullptr;
static HANDLE g_file = nullptr;
static HANDLE g_map  = nullptr;

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t p = s.rfind(L'\\');
    return p == std::wstring::npos ? L"" : s.substr(0, p + 1);
}

// True if the engine DLL is already loaded in dwm (marker published by the DLL).
static bool EngineLoaded() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\FringelessDwmEngine");
    if (m) { CloseHandle(m); return true; }
    return false;
}

// Run the bundled injector elevated and wait for it. Returns false if the user declined
// the UAC prompt or the helper could not be launched.
static bool RunInjector(const std::wstring& args) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    std::wstring exe = ExeDir() + L"fringeless_inject.exe";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 20000);
        CloseHandle(sei.hProcess);
    }
    return true;
}

static void MapParams() {
    if (g_params) return;
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\fringeless_params.bin", path, MAX_PATH);
    g_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) { g_file = nullptr; return; }
    g_map = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE, 0, sizeof(FringeParams), nullptr);
    if (!g_map) { CloseHandle(g_file); g_file = nullptr; return; }
    g_params = (FringeParams*)MapViewOfFile(g_map, FILE_MAP_WRITE, 0, 0, sizeof(FringeParams));
}

// Publish the current settings to the engine. Cheap; called on every settings change.
static void DwmEnginePush() {
    if (!g_params) return;
    for (int ch = 0; ch < 3; ch++) {
        g_params->sx[ch] = g_settings.shift[ch][kX].load();
        g_params->sy[ch] = g_settings.shift[ch][kY].load();
    }
    g_params->bicubic = g_settings.bicubic ? 1 : 0;
    g_params->enabled = g_settings.enabled ? 1 : 0;
    g_params->magic = kFringeMagic;
}

// Ensure the engine is running and take control of its parameter block. Injects (with one
// UAC prompt) only if it is not already loaded. Returns false if injection was declined
// or failed. Safe to call repeatedly.
static bool DwmEngineStart() {
    if (!EngineLoaded()) {
        std::wstring dll = ExeDir() + L"fringeless_dwm.dll";
        if (!RunInjector(L"inject \"" + dll + L"\"")) return false;
    }
    MapParams();
    if (!g_params) return false;
    DwmEnginePush();
    return true;
}

// Reconcile the engine with the current settings. Call after any settings change and at
// startup. Injects only on an off->on transition (one UAC prompt), never while merely
// adjusting sliders, then publishes the latest parameters.
void DwmEngineSync() {
    static bool wasEnabled = false;
    bool now = g_settings.enabled;
    if (now && !wasEnabled && !g_params) {
        DwmEngineStart();   // rising edge: bring the engine up (may prompt for UAC once)
    }
    wasEnabled = now;
    DwmEnginePush();
}

// Release our handles without touching the engine. Called when the app exits: the DLL
// stays in dwm (correction persists per the saved enable flag) and the next launch takes
// control again without another UAC prompt.
void DwmEngineShutdown() {
    if (g_params) { UnmapViewOfFile((void*)g_params); g_params = nullptr; }
    if (g_map)  { CloseHandle(g_map);  g_map = nullptr; }
    if (g_file) { CloseHandle(g_file); g_file = nullptr; }
}

// Fully unload the engine from dwm (one UAC prompt), e.g. for an explicit "unload".
void DwmEngineStop() {
    DwmEngineShutdown();
    if (EngineLoaded()) RunInjector(L"eject fringeless_dwm.dll");
}

bool DwmEngineReady() { return g_params != nullptr; }
