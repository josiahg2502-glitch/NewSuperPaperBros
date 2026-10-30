# Builds New Super Paper Bros. and updates the player's game folder (-Install, default Desktop\My recomps\NewSuperPaperBros).
#   powershell -ExecutionPolicy Bypass -File tools\package-spm.ps1 [-Build] [-Zip] [-Install <folder>]
# -Build first rebuilds the game (build-spm.ps1 -SkipTools) and the starter. -Zip also writes dist\NewSuperPaperBros.zip.
# Layout: New Super Paper Bros.exe (starter: runs engine\nspb-engine.exe), README.txt, engine\ (nspb-engine.exe = Play\SuperPaperMario.exe renamed,
# engine_usa.dll / engine_pal.dll = the recompiled game per disc region,
# DLLs, startup\ sprites, wii_bootstrap\, game_files\ = built-in HQ music, UserData\ = saves/settings/mods + HD textures).
# The disc is never copied in: the game sets itself up from the player's image into Disc\ inside the folder.
# A clean copy is staged in dist\_stage (deleted afterwards) and copied over the game folder, so the saves, Config.toml,
# mods and Disc\ already there are kept. The version is <Version> in launcher\NewSuperPaperBros.Launcher.csproj.
[CmdletBinding()] param([switch]$Build, [switch]$Zip, [string]$Install = "$env:USERPROFILE\Desktop\My recomps\NewSuperPaperBros")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$starter = "$PSScriptRoot\starter\out\New Super Paper Bros.exe"
if ($Build) { & "$PSScriptRoot\build-spm.ps1" -SkipTools }
& "$PSScriptRoot\starter\build-starter.ps1" | Out-Null
$version = 'v' + ([xml](Get-Content -LiteralPath "$root\launcher\NewSuperPaperBros.Launcher.csproj")).Project.PropertyGroup.Version
function Mirror([string]$From, [string]$To) {
    robocopy $From $To /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null  # few retries: a running game locks its files
    if ($LASTEXITCODE -ge 8) { throw "robocopy $From -> $To failed ($LASTEXITCODE)" }
}
function Remove-Tree([string]$Path) {
    # Explorer or antivirus briefly holds files in a 1 GB tree after it is opened: retry the delete.
    for ($try = 1; Test-Path -LiteralPath $Path; $try++) {
        try { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop }
        catch { if ($try -ge 5) { throw }; Start-Sleep -Seconds 3 }
    }
}

$stage = "$root\dist\_stage\NewSuperPaperBros"
Remove-Tree "$root\dist\_stage"
$engine = "$stage\engine"
New-Item -ItemType Directory -Force "$engine\UserData\mods" | Out-Null
Copy-Item -LiteralPath $starter -Destination $stage
# The game: runtime files only (no saves, logs, caches, debug *.disabled switches or the dev texture packs).
Copy-Item -LiteralPath "$root\Play\SuperPaperMario.exe" -Destination "$engine\nspb-engine.exe"
# The exe is the loader; the engine proper is one module per disc region (engine_usa.dll, engine_pal.dll from
# build-spm.ps1 -Pal), copied with the other DLLs below.
foreach ($m in 'engine_usa.dll', 'engine_pal.dll') { if (-not (Test-Path -LiteralPath "$root\Play\$m")) { throw "missing ${m}: run build-spm.ps1 (and -Pal)" } }
foreach ($f in '*.dll', 'dsp_coef.bin', 'initial_pipeline_cache.db', 'portable.txt') { Copy-Item "$root\Play\$f" $engine }
# Dawn (webgpu_dawn.dll) and dxcompiler.dll need the Visual C++ 2015-2022 runtime; ship it app-local (Microsoft's
# redistributable files) so a PC without the VC++ Redistributable still starts the game.
foreach ($f in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item "$env:SystemRoot\System32\$f" $engine }
foreach ($d in 'startup', 'wii_bootstrap') { Mirror "$root\Play\$d" "$engine\$d" }
# The built-in updater compares this with the latest GitHub release (runtime updater.cpp).
Set-Content -LiteralPath "$engine\version.txt" -Value $version -Encoding ascii
# Built-in HQ music: the mod's brstm files, which in the dev tree replaced the originals inside Disc\DATA\files\sound
# (Disc\sound_original holds the disc's own copies). Only files with a saved original are the mod's, never disc files.
$audio = "$engine\game_files\sound"
New-Item -ItemType Directory -Force $audio | Out-Null
Get-ChildItem -LiteralPath "$root\Disc\sound_original" -Filter *.brstm | ForEach-Object {
    $hq = "$root\Disc\DATA\files\sound\$($_.Name)"
    if ((Get-FileHash -LiteralPath $hq).Hash -eq (Get-FileHash -LiteralPath $_.FullName).Hash) { throw "$hq is the disc's own file, not the HQ mod's" }
    Copy-Item -LiteralPath $hq -Destination $audio
}
# Built-in HD textures: the 4x BC3 DDS pack, Dolphin texture_replacements naming.
Mirror "$root\Play\UserData\texture_replacements\SPM_HD_4x_BC3" "$engine\UserData\texture_replacements\SPM_HD_4x_BC3"
# On-screen button prompts per controller type (tools\make-button-icons.py), picked by the controller in use.
Mirror "$root\Play\UserData\button_icons" "$engine\UserData\button_icons"
# Credits and licenses: GPLv3 for the engine, the bundled libraries' own license files.
Copy-Item -LiteralPath "$PSScriptRoot\CREDITS.txt" -Destination "$stage\CREDITS.txt"
Copy-Item -LiteralPath "$root\wiicompiled\LICENSE" -Destination "$stage\LICENSE.txt"
# Code mods: the C interface and an example (README.txt "Mods").
New-Item -ItemType Directory -Force "$stage\mod-sdk" | Out-Null
Copy-Item -LiteralPath "$root\wiicompiled\runtime\include\spm_mod_api.h", "$PSScriptRoot\mod-sdk\example_mod.c" -Destination "$stage\mod-sdk"
New-Item -ItemType Directory -Force "$engine\licenses" | Out-Null
Copy-Item -LiteralPath "$root\wiicompiled\THIRD-PARTY-NOTICES.md" -Destination "$engine\licenses"
foreach ($dir in Get-ChildItem -LiteralPath "$root\wiicompiled\Dependencies" -Directory) {
    foreach ($f in Get-ChildItem -LiteralPath $dir.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' }) {
        Copy-Item -LiteralPath $f.FullName -Destination "$engine\licenses\$($dir.Name)-$($f.BaseName).txt"
    }
}
(Get-Content -LiteralPath "$PSScriptRoot\package-README.txt" -Raw).Replace('{VERSION}', $version) | Set-Content -LiteralPath "$stage\README.txt" -Encoding utf8

# Guard: no disc data comes from the dev tree (the player's own Disc\ is made by the game inside the folder).
if (Get-ChildItem -LiteralPath $stage -Recurse -Directory | Where-Object { $_.Name -in 'Disc', 'DATA', 'sys' }) { throw 'disc folders in the build' }

if ($Zip) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zipPath = "$root\dist\NewSuperPaperBros.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    # Entry by entry with '/' separators: Windows PowerShell's CreateFromDirectory writes '\', which 7-Zip, WinRAR,
    # Linux and macOS unpack as flat files named "NewSuperPaperBros\engine\..." (only Explorer copes).
    $base = Split-Path -Parent $stage
    $archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in Get-ChildItem -LiteralPath $stage -Recurse -Force) {
            $name = $item.FullName.Substring($base.Length + 1).Replace('\', '/')
            if (-not $item.PSIsContainer) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $item.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
            } elseif (-not (Get-ChildItem -LiteralPath $item.FullName -Force)) {
                [void]$archive.CreateEntry("$name/")  # keep empty folders (UserData\mods)
            }
        }
    } finally { $archive.Dispose() }
    Write-Host "ZIP: $zipPath"
}

# Copy over the game folder, keeping what is not in the build (saves, Config.toml, mods, Disc\).
$dist = $Install
# Generated, not the player's: replaced whole, so files a newer build no longer makes don't linger (a stale icon there
# wins over the current set).
Remove-Tree "$dist\engine\UserData\button_icons"
Mirror $stage $dist
# Earlier layouts: the game in Play\ (carry the player's files into engine\UserData) and the public-release extras.
if (Test-Path -LiteralPath "$dist\Play") {
    foreach ($item in 'Config.toml', 'Saves', 'NAND', 'mods') {
        $old = "$dist\Play\UserData\$item"
        if (Test-Path -LiteralPath $old) { if ((Get-Item -LiteralPath $old).PSIsContainer) { Mirror $old "$dist\engine\UserData\$item" } else { Copy-Item -LiteralPath $old -Destination "$dist\engine\UserData\" -Force } }
    }
    Remove-Tree "$dist\Play"
}
Remove-Tree "$dist\engine\game_files\README.txt"
Remove-Tree "$root\dist\_stage"
# Only the starter should be visible: hide the engine folder (the starter finds it regardless).
$eng = Get-Item -LiteralPath "$dist\engine" -Force; $eng.Attributes = $eng.Attributes -bor [IO.FileAttributes]::Hidden
$size = [math]::Round((Get-ChildItem -LiteralPath $dist -Recurse -File -Force | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "DONE: $dist ($version, $size MB)"
