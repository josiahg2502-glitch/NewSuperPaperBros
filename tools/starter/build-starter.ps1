# Builds the download's top-level "New Super Paper Bros.exe" (starter.c: starts engine\nspb-engine.exe) with the
# llvm-mingw toolchain build-spm.ps1 fetched. Output: tools\starter\out\New Super Paper Bros.exe
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $root 'toolchain\llvm-mingw\bin'
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $PSScriptRoot
try {
    & (Join-Path $bin 'x86_64-w64-mingw32-windres.exe') 'starter.rc' -O coff -o (Join-Path $out 'starter.res')
    if ($LASTEXITCODE -ne 0) { throw 'windres failed' }
    & (Join-Path $bin 'x86_64-w64-mingw32-clang.exe') -O2 -municode -mwindows 'starter.c' (Join-Path $out 'starter.res') -o (Join-Path $out 'New Super Paper Bros.exe')
    if ($LASTEXITCODE -ne 0) { throw 'clang failed' }
} finally { Pop-Location }
Write-Host "DONE: $out\New Super Paper Bros.exe"
