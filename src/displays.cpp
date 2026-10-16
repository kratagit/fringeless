// Display enumeration: DXGI outputs joined with monitor names from the display config.

#include "app.h"

#include <cwctype>
#include <map>

std::vector<Display> g_displays;
std::atomic<int>     g_target{ -1 };

bool TargetValid() {
    int t = g_target;
    return t >= 0 && t < (int)g_displays.size();
}

// GDI device name ("\\.\DISPLAY1") -> monitor name, from the active display paths.
static std::map<std::wstring, std::wstring> MonitorNames() {
    std::map<std::wstring, std::wstring> names;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return names;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return names;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0])
            names[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
    }
    return names;
}

void EnumerateDisplays() {
    std::map<std::wstring, std::wstring> names = MonitorNames();
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* out = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc = {};
            out->GetDesc(&desc);
            IDXGIOutput1* out1 = nullptr;
            if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1))) {
                Display d;
                d.adapter = adapter;
                adapter->AddRef();
                d.output = out1;
                auto it = names.find(desc.DeviceName);
                d.device = FileNameOf(desc.DeviceName);
                d.name = it != names.end() ? it->second : d.device;
                d.rect = desc.DesktopCoordinates;
                MONITORINFO mi = { sizeof(mi) };
                d.primary = GetMonitorInfoW(desc.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
                g_displays.push_back(d);
            }
            out->Release();
        }
        adapter->Release();
    }
    factory->Release();
}

void ReleaseDisplays() {
    for (Display& d : g_displays) {
        SafeRelease(d.output);
        SafeRelease(d.adapter);
    }
    g_displays.clear();
}

// The saved display, else the first OLED panel, else the primary display.
int PickDefaultDisplay() {
    for (size_t i = 0; i < g_displays.size(); ++i)
        if (!g_settings.display.empty() && g_displays[i].device == g_settings.display) return (int)i;
    for (size_t i = 0; i < g_displays.size(); ++i) {
        std::wstring upper = g_displays[i].name;
        for (wchar_t& c : upper) c = (wchar_t)towupper(c);
        if (upper.find(L"OLED") != std::wstring::npos) return (int)i;
    }
    for (size_t i = 0; i < g_displays.size(); ++i)
        if (g_displays[i].primary) return (int)i;
    return g_displays.empty() ? -1 : 0;
}

IDXGIOutput1* FindOutput(const std::wstring& device, IDXGIAdapter1** adapterOut) {
    if (adapterOut) *adapterOut = nullptr;
    if (device.empty()) return nullptr;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return nullptr;
    IDXGIOutput1* found = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* out = nullptr;
        for (UINT o = 0; !found && adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc = {};
            out->GetDesc(&desc);
            if (FileNameOf(desc.DeviceName) == device &&
                SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&found)) && adapterOut) {
                *adapterOut = adapter;
                adapter->AddRef();
            }
            out->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

std::wstring ProcessNameOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";
    std::wstring name;
    wchar_t path[MAX_PATH + 4] = {};
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        name = FileNameOf(path);
        for (wchar_t& c : name) c = (wchar_t)towlower(c);
    }
    CloseHandle(proc);
    return name;
}
