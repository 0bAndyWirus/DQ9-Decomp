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
if (-not $save -and -not $hasSave) { $save = Join-Path $env:APPDATA 'dq9-decomp\godot.sav' }
New-Item -ItemType Directory -Force -Path (Split-Path ([System.IO.Path]::GetFullPath($save)) -Parent) | Out-Null
$commandArgs = @('--path', (Join-Path $root 'godot'), '--', '--language', $language)
if (-not $hasSave) { $commandArgs += @('--save', $save) }
if ($rom -and -not $hasRom) { $commandArgs += @('--rom', $rom) }
if (-not $menu) { $commandArgs += '--autostart' }
$commandArgs += $launchArgs
Push-Location $root
try {
    # main.gd explicitly loads the extension on a cold project; no editor import is needed.
    try { & $godot @commandArgs }
    catch { throw "Could not start Godot executable '$godot'. Run BUILD-GODOT.bat or set GODOT to a working Godot 4.5 Windows x64 executable. $($_.Exception.Message)" }
    $code = $LASTEXITCODE
} finally { Pop-Location }
exit $code
