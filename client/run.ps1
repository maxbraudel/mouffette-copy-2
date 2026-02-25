param(
    [switch]$ConsoleLogs,
    [ValidateSet('default','d3d11','opengl','software')]
    [string]$RhiBackend = 'default',
    [switch]$MediaDebug
)

# Ensure Qt6 DLLs are on PATH
$ucrtBin = 'C:\msys64\ucrt64\bin'
$env:Path = "$ucrtBin;" + $env:Path

if ($ConsoleLogs) {
    $env:QT_LOGGING_RULES = "*.debug=true;qt.qpa.*=false"
}

if ($MediaDebug) {
    $env:QT_DEBUG_PLUGINS = '1'
    $env:QSG_INFO = '1'
    $env:QT_LOGGING_RULES = 'qt.multimedia.*=true;qt.quick.image=true;qt.scenegraph.general=true;qt.rhi.*=true'
}

switch ($RhiBackend.ToLowerInvariant()) {
    'd3d11'   { $env:QSG_RHI_BACKEND = 'd3d11' }
    'opengl'  { $env:QSG_RHI_BACKEND = 'opengl' }
    'software'{ $env:QSG_RHI_BACKEND = 'software' }
    default   { if (Test-Path Env:QSG_RHI_BACKEND) { Remove-Item Env:QSG_RHI_BACKEND } }
}

if (-not $env:QT_MEDIA_BACKEND) {
    $env:QT_MEDIA_BACKEND = 'ffmpeg'
}

if (-not $env:MOUFFETTE_USE_QUICK_CANVAS_RENDERER) {
    $env:MOUFFETTE_USE_QUICK_CANVAS_RENDERER = '1'
}

$clientExe = Join-Path $PSScriptRoot 'build\MouffetteClient.exe'
if (-not (Test-Path $clientExe)) {
    Write-Host "❌ Client not found. Build first with: $($PSScriptRoot)\build.ps1" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Mouffette client (Windows)" -ForegroundColor Cyan
if ($RhiBackend -ne 'default') {
    Write-Host "Using QSG_RHI_BACKEND=$($env:QSG_RHI_BACKEND)" -ForegroundColor Yellow
}
# Launch attached to this console for logs
& $clientExe
