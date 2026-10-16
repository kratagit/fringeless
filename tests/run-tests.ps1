# Builds and runs tests/verify.cpp (needs a Direct3D 11 GPU). Run from any directory.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)

$gxx = Get-Command g++ -ErrorAction SilentlyContinue
$bin = if ($gxx) { Split-Path $gxx.Source } else {
    Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin" -Directory |
        Select-Object -First 1 -ExpandProperty FullName
}
New-Item -ItemType Directory -Force build | Out-Null
& "$bin\g++.exe" -std=c++17 -O2 -Wall -static -o build\verify.exe tests\verify.cpp -ld3d11 -lgdi32 -luuid
if ($LASTEXITCODE) { throw 'g++ failed' }
& .\build\verify.exe
exit $LASTEXITCODE
