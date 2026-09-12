param(
    [switch]$ConsoleLogs,
    [ValidateSet('default', 'd3d11', 'opengl', 'software')]
    [string]$RhiBackend = 'default',
    [switch]$MediaDebug,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ApplicationArguments
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'internal\run-windows.ps1') `
    -Configuration Dev @PSBoundParameters
exit $LASTEXITCODE
