param(
    [switch]$Clean,
    [switch]$ConsoleLogs,
    [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'internal\build-windows.ps1') `
    -Configuration Prod @PSBoundParameters
exit $LASTEXITCODE
