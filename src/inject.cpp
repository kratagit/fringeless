// Elevated injector, built into the app itself. When Fringeless.exe is launched with
// --inject or --eject (which the app does for itself through ShellExecute "runas"), it
// extracts the engine DLL embedded in its own resources, publishes the shared parameter
// file, and loads or unloads the DLL in dwm.exe. Shipping one executable, not three.

#include "app.h"
#include "resource.h"
#include <tlhelp32.h>
#include <aclapi.h>

namespace {

#pragma pack(push, 4)
struct FringeParams {
    unsigned int magic; int enabled; float sx[3]; float sy[3];
    int bicubic; int matchMonitor; int left, top, width, height;
};
#pragma pack(pop)
const unsigned int kMagic = 0x46524E47u;
const wchar_t* kDllName = L"fringeless_dwm.dll";

DWORD FindProc(const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe); DWORD pid = 0;
    if (Process32FirstW(s, &pe)) do {
        if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    } while (Process32NextW(s, &pe));
    CloseHandle(s); return pid;
}
void EnableDebug() {
    HANDLE t; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return;
    LUID l; LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &l);
    TOKEN_PRIVILEGES tp; tp.PrivilegeCount = 1; tp.Privileges[0].Luid = l; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(t, FALSE, &tp, sizeof(tp), nullptr, nullptr); CloseHandle(t);
}
HMODULE RemoteMod(DWORD pid, const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W me; me.dwSize = sizeof(me); HMODULE h = nullptr;
    if (Module32FirstW(s, &me)) do {
        if (_wcsicmp(me.szModule, name) == 0) { h = me.hModule; break; }
    } while (Module32NextW(s, &me));
    CloseHandle(s); return h;
}
void NullDacl(const wchar_t* p) {
    SetNamedSecurityInfoW((LPWSTR)p, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr);
}

bool ExtractDll(const wchar_t* dest) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_DWMDLL), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r); DWORD sz = SizeofResource(nullptr, r);
    void* p = g ? LockResource(g) : nullptr;
    if (!p || !sz) return false;
    HANDLE f = CreateFileW(dest, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; WriteFile(f, p, sz, &w, nullptr); CloseHandle(f);
    return w == sz;
}

int DoInject() {
    wchar_t dll[MAX_PATH], pp[MAX_PATH];
    ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\fringeless_dwm.dll", dll, MAX_PATH);
    ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\fringeless_params.bin", pp, MAX_PATH);
    if (!ExtractDll(dll)) return 1;
    NullDacl(dll);

    // Publish the shared parameter file (NULL DACL), initialising only a fresh one.
    HANDLE pf = CreateFileW(pp, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pf != INVALID_HANDLE_VALUE) {
        FringeParams p; DWORD got = 0; ReadFile(pf, &p, sizeof(p), &got, nullptr);
        if (got != sizeof(p) || p.magic != kMagic) {
            FringeParams init{}; init.magic = kMagic; init.enabled = 1;
            init.sx[1] = -0.146f; init.sy[1] = 0.494f; init.sx[2] = -0.011f;
            SetFilePointer(pf, 0, nullptr, FILE_BEGIN); DWORD w; WriteFile(pf, &init, sizeof(init), &w, nullptr);
        }
        CloseHandle(pf); NullDacl(pp);
    }

    EnableDebug();
    DWORD pid = FindProc(L"dwm.exe"); if (!pid) return 1;
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) return 1;
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* rem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) { CloseHandle(proc); return 1; }
    WriteProcessMemory(proc, rem, dll, bytes, nullptr);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(k, "LoadLibraryW"), rem, 0, nullptr);
    if (th) { WaitForSingleObject(th, 10000); CloseHandle(th); }
    VirtualFreeEx(proc, rem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return 0;
}

int DoEject() {
    EnableDebug();
    DWORD pid = FindProc(L"dwm.exe"); if (!pid) return 1;
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ |
                              PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) return 1;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    for (int i = 0; i < 32; i++) {              // FreeLibrary until the module is gone
        HMODULE h = RemoteMod(pid, kDllName); if (!h) break;
        HANDLE th = CreateRemoteThread(proc, nullptr, 0,
            (LPTHREAD_START_ROUTINE)GetProcAddress(k, "FreeLibrary"), h, 0, nullptr);
        if (!th) break;
        WaitForSingleObject(th, 5000); CloseHandle(th);
    }
    CloseHandle(proc);
    return 0;
}

} // namespace

// Returns the process exit code for an --inject/--eject launch, or -1 when the command line
// is a normal launch and the app should start its UI.
int InjectModeMain(const wchar_t* cmdLine) {
    if (!cmdLine) return -1;
    if (wcsstr(cmdLine, L"--inject")) return DoInject();
    if (wcsstr(cmdLine, L"--eject")) return DoEject();
    return -1;
}
