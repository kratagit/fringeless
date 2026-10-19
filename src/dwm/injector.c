// Fringeless DWM injector helper.
//   injector.exe inject <full\path\to\fringeless_dwm.dll>
//   injector.exe eject  <fringeless_dwm.dll>
// Must run elevated. Injects the correction DLL into dwm.exe via LoadLibraryW in a remote
// thread. dwm.exe runs as the "Window Manager\DWM-N" virtual account and cannot read the
// user profile, so the DLL and the shared parameter file are staged in %SYSTEMROOT%\Temp
// and given a NULL DACL (as dwm_lut's injector does with SetSecurityInfo) so dwm can open
// them. This file is part of Fringeless (GNU GPL v3; see LICENSE).
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <stdio.h>

#pragma pack(push, 4)
struct FringeParams {
    unsigned int magic;   // 'FRNG' once valid
    int          enabled;
    float        sx[3];
    float        sy[3];
    int          bicubic;
};
#pragma pack(pop)

static DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static int EnableDebugPriv(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return 0;
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) { CloseHandle(tok); return 0; }
    TOKEN_PRIVILEGES tp; tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(tok);
    return ok && GetLastError() == ERROR_SUCCESS;
}

static HMODULE RemoteModule(DWORD pid, const wchar_t* dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32W me; me.dwSize = sizeof(me);
    HMODULE h = NULL;
    if (Module32FirstW(snap, &me)) {
        do { if (_wcsicmp(me.szModule, dllName) == 0) { h = me.hModule; break; } }
        while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return h;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: injector inject <full dll path> | eject <dll name>\n"); return 2; }
    EnableDebugPriv();
    const wchar_t* target = (argc >= 4) ? argv[3] : L"dwm.exe";
    DWORD pid = FindProcess(target);
    if (!pid) { wprintf(L"%ls not found\n", target); return 1; }
    wprintf(L"%ls pid = %lu\n", target, pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed: %lu (run elevated?)\n", GetLastError()); return 1; }

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig = {0};
    if (GetProcessMitigationPolicy(proc, ProcessSignaturePolicy, &sig, sizeof(sig)))
        wprintf(L"dwm SignaturePolicy: MicrosoftSignedOnly=%u StoreSignedOnly=%u MitigationOptIn=%u\n",
                sig.MicrosoftSignedOnly, sig.StoreSignedOnly, sig.MitigationOptIn);
    else
        wprintf(L"GetProcessMitigationPolicy failed: %lu\n", GetLastError());

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");

    if (_wcsicmp(argv[1], L"inject") == 0) {
        // dwm.exe runs as the "Window Manager\DWM-N" virtual account, which cannot read
        // the user profile. Copy the DLL to C:\Windows\Temp and give it a NULL DACL
        // (everyone may read) so dwm's remote LoadLibrary can open it. (Same as dwm_lut.)
        const wchar_t* src = argv[2];
        const wchar_t* srcBase = wcsrchr(src, L'\\');
        srcBase = srcBase ? srcBase + 1 : src;
        static wchar_t dllBuf[MAX_PATH];
        wchar_t tempDir[MAX_PATH];
        ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\", tempDir, MAX_PATH);
        _snwprintf(dllBuf, MAX_PATH, L"%s%s", tempDir, srcBase);
        if (!CopyFileW(src, dllBuf, FALSE)) {
            wprintf(L"CopyFile to %ls failed: %lu\n", dllBuf, GetLastError());
            CloseHandle(proc); return 1;
        }
        // NULL DACL => accessible to every account, including the DWM virtual account.
        DWORD sec = SetNamedSecurityInfoW(dllBuf, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                          NULL, NULL, NULL, NULL);
        if (sec != ERROR_SUCCESS) wprintf(L"SetNamedSecurityInfo warning: %lu\n", sec);
        wprintf(L"staged DLL at %ls\n", dllBuf);
        // Route the DLL's diagnostic log to a readable file (NULL DACL so dwm can write).
        {
            wchar_t logPathFile[MAX_PATH], logTarget[MAX_PATH];
            ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\dwm_lut_log_path.txt", logPathFile, MAX_PATH);
            ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\fringeless.log", logTarget, MAX_PATH);
            HANDLE lf = CreateFileW(logPathFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (lf != INVALID_HANDLE_VALUE) {
                char a[MAX_PATH]; int k = 0;
                for (; logTarget[k] && k < MAX_PATH - 1; k++) a[k] = (char)logTarget[k];
                DWORD wr; WriteFile(lf, a, (DWORD)k, &wr, NULL); CloseHandle(lf);
                SetNamedSecurityInfoW(logPathFile, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
            }
        }
        // Create the shared parameter file (NULL DACL) so the non-elevated app can map and
        // write it. Keep any existing values; only initialise a fresh/zero file.
        {
            wchar_t paramsPath[MAX_PATH];
            ExpandEnvironmentStringsW(L"%SYSTEMROOT%\\Temp\\fringeless_params.bin", paramsPath, MAX_PATH);
            HANDLE pf = CreateFileW(paramsPath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
            if (pf != INVALID_HANDLE_VALUE) {
                struct FringeParams p; DWORD got = 0;
                ReadFile(pf, &p, sizeof(p), &got, NULL);
                if (got != sizeof(p) || p.magic != 0x46524E47u) {
                    struct FringeParams init = {0};
                    init.magic = 0x46524E47u; init.enabled = 1;
                    init.sx[1] = -0.146f; init.sy[1] = 0.494f; init.sx[2] = -0.011f;
                    SetFilePointer(pf, 0, NULL, FILE_BEGIN);
                    DWORD wr; WriteFile(pf, &init, sizeof(init), &wr, NULL);
                }
                CloseHandle(pf);
                SetNamedSecurityInfoW(paramsPath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
            }
        }
        const wchar_t* dll = dllBuf;
        SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) { wprintf(L"VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(proc); return 1; }
        if (!WriteProcessMemory(proc, remote, dll, bytes, NULL)) {
            wprintf(L"WriteProcessMemory failed: %lu\n", GetLastError()); CloseHandle(proc); return 1;
        }
        LPTHREAD_START_ROUTINE loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, NULL, 0, loadLib, remote, 0, NULL);
        if (!th) { wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError()); CloseHandle(proc); return 1; }
        WaitForSingleObject(th, 10000);
        DWORD code = 0; GetExitCodeThread(th, &code);
        CloseHandle(th);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        Sleep(300);
        const wchar_t* base = wcsrchr(dll, L'\\');
        base = base ? base + 1 : dll;
        HMODULE loaded = RemoteModule(pid, base);
        if (loaded) wprintf(L"VERIFY: %ls IS loaded in dwm (base=0x%p)\n", base, loaded);
        else        wprintf(L"VERIFY: %ls NOT loaded in dwm (thread exit low32=0x%lX)\n", base, code);
    } else if (_wcsicmp(argv[1], L"eject") == 0) {
        HMODULE h = RemoteModule(pid, argv[2]);
        if (!h) { wprintf(L"module %ls not found in dwm.exe\n", argv[2]); CloseHandle(proc); return 1; }
        LPTHREAD_START_ROUTINE freeLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "FreeLibrary");
        HANDLE th = CreateRemoteThread(proc, NULL, 0, freeLib, h, 0, NULL);
        if (!th) { wprintf(L"CreateRemoteThread(FreeLibrary) failed: %lu\n", GetLastError()); CloseHandle(proc); return 1; }
        WaitForSingleObject(th, 10000);
        CloseHandle(th);
        wprintf(L"eject done\n");
    } else {
        wprintf(L"unknown command %ls\n", argv[1]);
    }
    CloseHandle(proc);
    return 0;
}
