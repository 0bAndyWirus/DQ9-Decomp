# Usage: tools\run_game.ps1 [--menu] [en|fr|de|es|it] [--rom PATH] [dq9 options...]
$ErrorActionPreference = 'Stop'
$root = if (Test-Path (Join-Path $PSScriptRoot 'dq9.exe')) { $PSScriptRoot } else { Split-Path $PSScriptRoot -Parent }
$launchArgs = @($args)
$menu = $launchArgs.Count -gt 0 -and $launchArgs[0] -in @('--menu', '-Menu')
if ($menu) { $launchArgs = @($launchArgs | Select-Object -Skip 1) }
$language = 'en'
if ($launchArgs.Count -gt 0 -and $launchArgs[0] -in @('en', 'fr', 'de', 'es', 'it')) {
    $language = $launchArgs[0]
    $launchArgs = @($launchArgs | Select-Object -Skip 1)
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
$binary = $env:DQ9_EXE
if (-not $binary) {
    foreach ($candidate in @('dq9.exe', 'build\win-sdl\game\dq9.exe', 'build\win-native\game\Release\dq9.exe', 'build\win-native\game\dq9.exe')) {
        $path = Join-Path $root $candidate
        if (Test-Path $path) { $binary = $path; break }
    }
}
if (-not $binary -or -not (Test-Path $binary)) { throw 'dq9.exe is missing. Build it first; see docs/BUILDING_WINDOWS.md (or set DQ9_EXE).' }
if ($menu) {
    $commandArgs = @('--launcher')
} else {
    if (-not $rom) { throw 'Select an already available compatible image with --rom PATH or DQ9_ROM, or use --menu.' }
    if (-not $save -and -not $hasSave) { $save = Join-Path $env:APPDATA 'dq9-decomp\dq9.sav' }
    $commandArgs = @('--language', $language, '--debug-tool')
}
if ($save) {
    $parent = Split-Path ([System.IO.Path]::GetFullPath($save)) -Parent
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if (-not $hasSave) { $commandArgs += @('--save', $save) }
}
if ($rom -and -not $hasRom) { $commandArgs += @('--rom', $rom) }
$commandArgs += $launchArgs
Push-Location $root
try {
    & $binary @commandArgs
    $code = $LASTEXITCODE
} finally { Pop-Location }
exit $code
