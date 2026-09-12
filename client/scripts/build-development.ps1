param(
    [switch]$Clean,
    [switch]$ConsoleLogs,
    [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'internal\build-windows.ps1') `
    -Configuration Dev @PSBoundParameters
exit $LASTEXITCODE
