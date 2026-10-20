# Builds build\Fringeless.exe with MinGW-w64 (g++ and windres on PATH, or WinLibs from winget).
# The whole product is one executable: the DWM engine DLL is built first and embedded into
# Fringeless.exe as a resource, which extracts and injects it at runtime.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$gxx = Get-Command g++ -ErrorAction SilentlyContinue
if ($gxx) {
    $bin = Split-Path $gxx.Source
} else {
    $bin = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin" -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $bin) { throw 'g++ not found. Install MinGW-w64, e.g.: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
}

New-Item -ItemType Directory -Force build\nomanifest | Out-Null

# --- DWM correction engine DLL (derived from dwm_lut, GPL-3.0; see src/dwm and LICENSE) ---
# Built first so it can be embedded into the executable below.
$mh = 'third_party\minhook'
$mhObjs = @()
foreach ($c in @("$mh\src\buffer.c", "$mh\src\hook.c", "$mh\src\trampoline.c",
                 "$mh\src\hde\hde32.c", "$mh\src\hde\hde64.c")) {
    $o = 'build\mh_' + [IO.Path]::GetFileNameWithoutExtension($c) + '.o'
    & "$bin\gcc.exe" -O2 -c "-I$mh\include" -o $o $c
    if ($LASTEXITCODE) { throw "minhook compile failed: $c" }
    $mhObjs += $o
}
& "$bin\ar.exe" rcs build\libminhook.a @mhObjs
if ($LASTEXITCODE) { throw 'minhook archive failed' }

& "$bin\g++.exe" -std=c++17 -O2 -c "-I$mh\include" -Isrc\dwm -o build\dwm_dllmain.o src\dwm\dllmain.cpp
if ($LASTEXITCODE) { throw 'dwm dll compile failed' }
& "$bin\g++.exe" -shared -O2 -o build\fringeless_dwm.dll build\dwm_dllmain.o -Lbuild -lminhook `
    -static -static-libgcc -static-libstdc++ `
    -ld3d11 -ld3dcompiler -ldxgi -ldxguid -lpsapi -lgdi32 -luuid -lole32
if ($LASTEXITCODE) { throw 'dwm dll link failed' }
& "$bin\strip.exe" build\fringeless_dwm.dll

# --- App (embeds the DLL via Fringeless.rc; -I build lets windres find it) ---
& "$bin\windres.exe" -I src -I build -O coff -o build\Fringeless.res src\Fringeless.rc
if ($LASTEXITCODE) { throw 'windres failed' }

# GCC links mingw's default-manifest.o, which clashes with our own manifest resource.
# An empty object of the same name found first via -B replaces it.
'' | Set-Content build\nomanifest\empty.c
& "$bin\gcc.exe" -c -o build\nomanifest\default-manifest.o build\nomanifest\empty.c
if ($LASTEXITCODE) { throw 'gcc failed' }

$sources = Get-ChildItem src\*.cpp | ForEach-Object { $_.FullName }
$flags = @(
    '-Bbuild\nomanifest\', '-std=c++17', '-O2', '-Wall', '-municode', '-mwindows', '-static',
    '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DUNICODE', '-D_UNICODE'
)
$libs = @('-ld3d11', '-ldxgi', '-ld2d1', '-ldwrite', '-ldwmapi', '-lcomctl32', '-lgdi32', '-luser32', '-ladvapi32',
          '-lshell32', '-lole32', '-luuid')
& "$bin\g++.exe" @flags -o build\Fringeless.exe @sources build\Fringeless.res @libs
if ($LASTEXITCODE) { throw 'g++ failed' }

& "$bin\strip.exe" build\Fringeless.exe
Get-Item build\Fringeless.exe | Select-Object Name, Length
