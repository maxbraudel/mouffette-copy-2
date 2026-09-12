param(
    [ValidateSet('Dev', 'Prod')]
    [string]$Configuration = 'Dev',
    [switch]$Packaged,
    [switch]$ConsoleLogs,
    [ValidateSet('default', 'd3d11', 'opengl', 'software')]
    [string]$RhiBackend = 'default',
    [switch]$MediaDebug,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ApplicationArguments
)

$ErrorActionPreference = 'Stop'
$clientRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ($ConsoleLogs) {
    $env:QT_LOGGING_RULES = '*.debug=true;qt.qpa.*=false'
}
if ($MediaDebug) {
    $env:QT_DEBUG_PLUGINS = '1'
    $env:QSG_INFO = '1'
    $env:QT_LOGGING_RULES = 'qt.multimedia.*=true;qt.quick.image=true;qt.scenegraph.general=true;qt.rhi.*=true'
}

switch ($RhiBackend) {
    'd3d11'    { $env:QSG_RHI_BACKEND = 'd3d11' }
    'opengl'   { $env:QSG_RHI_BACKEND = 'opengl' }
    'software' { $env:QSG_RHI_BACKEND = 'software' }
    default    { Remove-Item Env:QSG_RHI_BACKEND -ErrorAction SilentlyContinue }
}

if ($Packaged) {
    # Deliberately do not add MSYS2/Qt to PATH: this validates the standalone tree.
    $clientExe = Join-Path $clientRoot 'out\stage\windows-release\bin\Mouffette.exe'
    $label = 'packaged production'
} else {
    $msysRoot = if ($env:MOUFFETTE_MSYS2_ROOT) { $env:MOUFFETTE_MSYS2_ROOT } else { 'C:\msys64' }
    $ucrtBin = Join-Path $msysRoot 'ucrt64\bin'
    if (-not (Test-Path $ucrtBin)) { throw "MSYS2 UCRT64 not found at $ucrtBin" }
    $env:Path = "$ucrtBin;$env:Path"

    $preset = if ($Configuration -eq 'Prod') { 'windows-release' } else { 'windows-debug' }
    $clientExe = Join-Path $clientRoot "out\build\$preset\Mouffette.exe"
    $label = if ($Configuration -eq 'Prod') { 'production Release' } else { 'development' }
}

if (-not (Test-Path $clientExe)) {
    $command = if ($Packaged) { '.\scripts\package-release.ps1' } else { '.\scripts\build-development.ps1 or .\scripts\build-release.ps1' }
    throw "Mouffette $label not found at $clientExe. Run: $command"
}

Write-Host "Starting Mouffette ($label)..." -ForegroundColor Cyan
& $clientExe @ApplicationArguments
exit $LASTEXITCODE
