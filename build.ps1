# Builds build\Fringeless.exe with MinGW-w64 (g++ and windres on PATH, or WinLibs from winget).
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

& "$bin\windres.exe" -I src -O coff -o build\Fringeless.res src\Fringeless.rc
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
Get-Item build\Fringeless.exe | Select-Object FullName, Length
