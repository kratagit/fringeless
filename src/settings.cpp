// Settings persistence: %APPDATA%\Fringeless\settings.ini.

#include "app.h"

#include <shlobj.h>

#include <cmath>
#include <cwchar>

Settings g_settings;

static const wchar_t* const kSection = L"Fringeless";
static const wchar_t* const kShiftKeys[3][2] = {
    { L"red_x", L"red_y" }, { L"green_x", L"green_y" }, { L"blue_x", L"blue_y" },
};

Settings::Settings() {
    neverPause =
        L"chrome.exe;firefox.exe;msedge.exe;brave.exe;opera.exe;vivaldi.exe;code.exe;devenv.exe;"
        L"notepad.exe;notepad++.exe;sublime_text.exe;windowsterminal.exe;wt.exe;conhost.exe;"
        L"powershell.exe;pwsh.exe;cmd.exe;explorer.exe;acrord32.exe;sumatrapdf.exe;obsidian.exe";
    ResetShifts();
}

void Settings::ResetShifts() {
    for (auto& channel : shift)
        for (auto& v : channel) v = 0.0f;
}

std::wstring SettingsFolder() {
    std::wstring dir;
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = std::wstring(appData) + L"\\" APP_NAME;
        CoTaskMemFree(appData);
    } else {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        dir = path;
        dir.resize(dir.rfind(L'\\'));
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring SettingsPath() { return SettingsFolder() + L"\\settings.ini"; }

static std::wstring ReadString(const std::wstring& ini, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(kSection, key, fallback, buf, 2048, ini.c_str());
    return buf;
}

static bool ReadBool(const std::wstring& ini, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(kSection, key, fallback ? 1 : 0, ini.c_str()) != 0;
}

static float ReadFloat(const std::wstring& ini, const wchar_t* key, float fallback, float lo, float hi) {
    std::wstring s = ReadString(ini, key, L"");
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str()) return fallback;
    return (float)std::fmin(std::fmax(v, (double)lo), (double)hi);
}

void LoadSettings() {
    std::wstring ini = SettingsPath();
    Settings& s = g_settings;
    for (int c = 0; c < 3; ++c)
        for (int a = 0; a < 2; ++a) {
            float v = ReadFloat(ini, kShiftKeys[c][a], s.shift[c][a], -1.0f, 1.0f);
            s.shift[c][a] = std::round(v * 1000.0f) / 1000.0f;
        }
    s.enabled = ReadBool(ini, L"enabled", true);
    s.bicubic = ReadString(ini, L"interpolation", L"bicubic") != L"bilinear";
    s.pauseInFullscreen = ReadBool(ini, L"pause_in_fullscreen", true);
    std::wstring list = ReadString(ini, L"never_pause", L"");
    if (!list.empty()) s.neverPause = list;
    s.display = ReadString(ini, L"display", L"");

    int key = (int)GetPrivateProfileIntW(kSection, L"shortcut_key", -1, ini.c_str());
    if (key >= 0) {
        s.shortcutKey = (UINT)key;
        s.shortcutModifiers = GetPrivateProfileIntW(kSection, L"shortcut_modifiers", 0, ini.c_str()) &
                              (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
        s.shortcutChosen = true;
    }
}

void SaveSettings() {
    std::wstring ini = SettingsPath();
    const Settings& s = g_settings;
    wchar_t buf[32];
    auto put = [&](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(kSection, key, value.c_str(), ini.c_str());
    };
    put(L"display", s.display);
    put(L"enabled", s.enabled ? L"1" : L"0");
    for (int c = 0; c < 3; ++c)
        for (int a = 0; a < 2; ++a) {
            swprintf(buf, 32, L"%.3f", (double)s.shift[c][a]);
            put(kShiftKeys[c][a], buf);
        }
    put(L"interpolation", s.bicubic ? L"bicubic" : L"bilinear");
    put(L"pause_in_fullscreen", s.pauseInFullscreen ? L"1" : L"0");
    put(L"never_pause", s.neverPause);
    if (s.shortcutChosen) {
        put(L"shortcut_modifiers", std::to_wstring(s.shortcutModifiers));
        put(L"shortcut_key", std::to_wstring(s.shortcutKey));
    }
}

// ---------------------------------------------------------------------------
// Start with Windows (HKCU Run key)
// ---------------------------------------------------------------------------

static const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring StartupCommand() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\" --tray";
}

bool StartsWithWindows() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

void SetStartWithWindows(bool on) {
    if (on) {
        std::wstring cmd = StartupCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, REG_SZ, cmd.c_str(),
                        (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME);
    }
}

// Keeps the startup entry pointing at this executable if it was moved.
void RefreshStartupEntry() {
    if (StartsWithWindows()) SetStartWithWindows(true);
}
