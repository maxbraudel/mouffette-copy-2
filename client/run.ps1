param(
    [switch]$ConsoleLogs
)

# Ensure Qt6 DLLs are on PATH
$ucrtBin = 'C:\msys64\ucrt64\bin'
$env:Path = "$ucrtBin;" + $env:Path

if ($ConsoleLogs) {
    $env:QT_LOGGING_RULES = "*.debug=true;qt.qpa.*=false"
}

$clientExe = Join-Path $PSScriptRoot 'build\MouffetteClient.exe'
if (-not (Test-Path $clientExe)) {
    Write-Host "❌ Client not found. Build first with: $($PSScriptRoot)\build.ps1" -ForegroundColor Red
    exit 1
}

Write-Host "Starting Mouffette client (Windows)" -ForegroundColor Cyan
# Launch attached to this console for logs
& $clientExe
