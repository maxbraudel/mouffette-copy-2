param(
    [ValidateSet('Dev', 'Prod')]
    [string]$Configuration = 'Dev',
    [switch]$Clean,
    [switch]$ConsoleLogs,
    [string]$Target = ''
)

$ErrorActionPreference = 'Stop'
$clientRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
Set-Location $clientRoot

$msysRoot = if ($env:MOUFFETTE_MSYS2_ROOT) { $env:MOUFFETTE_MSYS2_ROOT } else { 'C:\msys64' }
$ucrtRoot = Join-Path $msysRoot 'ucrt64'
$ucrtBin = Join-Path $ucrtRoot 'bin'
if (-not (Test-Path $ucrtBin)) {
    throw "MSYS2 UCRT64 not found at $ucrtBin. Install MSYS2 and the Qt 6 UCRT64 packages documented in README.md."
}

$requiredPackages = @(
    (Join-Path $ucrtRoot 'lib\cmake\Qt6\Qt6Config.cmake'),
    (Join-Path $ucrtRoot 'lib\cmake\Qt6Quick\Qt6QuickConfig.cmake')
)
foreach ($requiredPackage in $requiredPackages) {
    if (-not (Test-Path $requiredPackage)) {
        throw "Required Qt package not found: $requiredPackage"
    }
}

$env:MOUFFETTE_QT_ROOT = $ucrtRoot
$env:Path = "$ucrtBin;$env:Path"

# Run the same architecture guardrail on Windows through MSYS2's bash.
$bash = Join-Path $msysRoot 'usr\bin\bash.exe'
if (Test-Path $bash) {
    # Avoid passing a nested `$()` expression through PowerShell's native
    # argument quoting. Convert the script path first, then execute it directly.
    $cygpath = Join-Path $msysRoot 'usr\bin\cygpath.exe'
    $guardScript = Join-Path $clientRoot 'tools\check_architecture_boundaries.sh'
    $guardScriptMsys = (& $cygpath -u $guardScript).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not convert the architecture guardrail path for MSYS2.' }

    & $bash -l $guardScriptMsys
    if ($LASTEXITCODE -ne 0) { throw 'Architecture boundary checks failed.' }
} else {
    Write-Warning "MSYS2 bash not found at $bash; architecture guardrail was not run."
}

$preset = if ($Configuration -eq 'Prod') { 'windows-release' } else { 'windows-debug' }
$label = if ($Configuration -eq 'Prod') { 'production (Release)' } else { 'development (Debug)' }
Write-Host "Building Mouffette for Windows $label..." -ForegroundColor Cyan

$configureArgs = @('--preset', $preset)
if ($Clean) { $configureArgs += '--fresh' }
if ($ConsoleLogs) { $configureArgs += '-DCONSOLE_OUTPUT=ON' }
& cmake @configureArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

$buildArgs = @('--build', '--preset', $preset, '--parallel')
if ($Target) { $buildArgs += @('--target', $Target) }
& cmake @buildArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

Write-Host "Build successful: out\build\$preset\Mouffette.exe" -ForegroundColor Green
Write-Host 'Use scripts\package-release.ps1 to create the standalone Release ZIP.' -ForegroundColor DarkGray
