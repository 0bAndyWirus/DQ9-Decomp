# Usage: tools\run_godot.ps1 [--menu] [en|fr|de|es|it] [--rom PATH] [Godot user options...]
$ErrorActionPreference = 'Stop'
$root = if (Test-Path (Join-Path $PSScriptRoot 'godot\project.godot')) { $PSScriptRoot } else { Split-Path $PSScriptRoot -Parent }
$launchArgs = @($args)
$menu = $launchArgs.Count -gt 0 -and $launchArgs[0] -in @('--menu', '-Menu')
if ($menu) { $launchArgs = @($launchArgs | Select-Object -Skip 1) }
$language = 'en'
if ($launchArgs.Count -gt 0 -and $launchArgs[0] -in @('en', 'fr', 'de', 'es', 'it')) {
    $language = $launchArgs[0]
    $launchArgs = @($launchArgs | Select-Object -Skip 1)
}
$godot = $env:GODOT
if (-not $godot) {
    # BUILD-GODOT.bat downloads a checksum-verified editor here.
    $localEditor = Join-Path $root 'build\tools\godot\Godot_v4.5-stable_win64.exe'
    if (Test-Path $localEditor) { $godot = $localEditor }
}
if (-not $godot) {
    $command = Get-Command godot.exe, godot4.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $godot = $command.Source }
}
if (-not $godot -or -not (Get-Command $godot -ErrorAction SilentlyContinue)) { throw 'No Godot 4.5 editor found. Run BUILD-GODOT.bat (downloads it), or set GODOT to your Godot 4.5 Windows x64 executable.' }
foreach ($dll in @('dq9_godot.windows.template_release.x86_64.dll', 'dsr_session.dll')) {
    if (-not (Test-Path (Join-Path $root "godot\bin\$dll"))) { throw "Missing godot\bin\$dll. Run BUILD-GODOT.bat first (after BUILD-DQ9.bat)." }
}
$rom = $env:DQ9_ROM
$save = $env:DQ9_SAVE
$hasRom = $false
$hasSave = $false
for ($i = 0; $i -lt $launchArgs.Count; $i++) {
    if ($launchArgs[$i] -in @('--rom', '--save')) {
        $option = $launchArgs[$i]
        if ($i + 1 -ge $launchArgs.Count) { throw "$option requires a value." }
        $i++
        if ($option -eq '--rom') { $rom = $launchArgs[$i]; $hasRom = $true }
        else { $save = $launchArgs[$i]; $hasSave = $true }
    }
}
if (-not $rom -and -not $menu) { throw 'Select an already available compatible image with --rom PATH or DQ9_ROM, or use --menu.' }
if (-not $save -and -not $hasSave -and -not $menu) {
    $save = Join-Path $env:APPDATA 'dq9-remastered\godot.sav'
    $legacySave = Join-Path $env:APPDATA 'dq9-decomp\godot.sav'
    if (-not (Test-Path -LiteralPath $save -PathType Leaf) -and (Test-Path -LiteralPath $legacySave -PathType Leaf)) { $save = $legacySave }
}
if ($save) { New-Item -ItemType Directory -Force -Path (Split-Path ([System.IO.Path]::GetFullPath($save)) -Parent) | Out-Null }
$commandArgs = @('--path', (Join-Path $root 'godot'), '--', '--language', $language)
if (-not $hasSave -and $save) { $commandArgs += @('--save', $save) }
if ($rom -and -not $hasRom) { $commandArgs += @('--rom', $rom) }
if (-not $menu) { $commandArgs += '--autostart' }
$commandArgs += $launchArgs
Push-Location $root
try {
    # The runtime needs the imported asset cache even though it loads the
    # extension itself. A cache directory alone may be incomplete.
    $project = Join-Path $root 'godot'
    $needImport = $false
    $assets = Join-Path $project 'assets'
    $metadataFiles = if (Test-Path -LiteralPath $assets -PathType Container) { Get-ChildItem -LiteralPath $assets -Filter '*.import' -Recurse -File }
    foreach ($metadata in $metadataFiles) {
        $resource = Get-Content -LiteralPath $metadata.FullName | Where-Object { $_ -match '^path="res://(.+)"$' } | Select-Object -First 1
        if (-not $resource -or $resource -notmatch '^path="res://(.+)"$' -or -not (Test-Path -LiteralPath (Join-Path $project $Matches[1]) -PathType Leaf)) {
            $needImport = $true
            break
        }
    }
    try {
        if ($needImport) {
            # Avoid Godot's cold extension-documentation crash by importing
            # identical res:// asset paths in a project without the extension.
            $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("dq9-godot-import-" + [guid]::NewGuid().ToString('N'))
            try {
                New-Item -ItemType Directory -Path $scratch | Out-Null
                Copy-Item -LiteralPath $assets -Destination (Join-Path $scratch 'assets') -Recurse
                Set-Content -LiteralPath (Join-Path $scratch 'project.godot') -Value 'config_version=5' -Encoding ascii
                & $godot --headless --path $scratch --editor --import --quit
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
                $imported = Join-Path $scratch '.godot\imported'
                if (-not (Test-Path -LiteralPath $imported -PathType Container)) { throw 'Godot did not create an imported resource cache.' }
                $destination = Join-Path $project '.godot\imported'
                New-Item -ItemType Directory -Force -Path $destination | Out-Null
                foreach ($entry in Get-ChildItem -LiteralPath $imported -Force) {
                    Copy-Item -LiteralPath $entry.FullName -Destination $destination -Recurse -Force
                }
            } finally {
                if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
            }
            foreach ($metadata in $metadataFiles) {
                $resource = Get-Content -LiteralPath $metadata.FullName | Where-Object { $_ -match '^path="res://(.+)"$' } | Select-Object -First 1
                if (-not $resource -or $resource -notmatch '^path="res://(.+)"$' -or -not (Test-Path -LiteralPath (Join-Path $project $Matches[1]) -PathType Leaf)) {
                    throw "Godot failed to import $($metadata.FullName)."
                }
            }
        }
        & $godot @commandArgs
    }
    catch { throw "Could not start Godot executable '$godot'. Run BUILD-GODOT.bat or set GODOT to a working Godot 4.5 Windows x64 executable. $($_.Exception.Message)" }
    $code = $LASTEXITCODE
} finally { Pop-Location }
exit $code
