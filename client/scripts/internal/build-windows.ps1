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

$cmake = Join-Path $ucrtBin 'cmake.exe'
if (-not (Test-Path $cmake -PathType Leaf)) {
    throw "CMake not found at $cmake. In the MSYS2 UCRT64 terminal, run: pacman -S --needed mingw-w64-ucrt-x86_64-cmake"
}

$requiredPackages = @(
    (Join-Path $ucrtRoot 'lib\cmake\Qt6\Qt6Config.cmake'),
    (Join-Path $ucrtRoot 'lib\cmake\Qt6Quick\Qt6QuickConfig.cmake'),
    (Join-Path $ucrtRoot 'include\winrt\base.h')
)
foreach ($requiredPackage in $requiredPackages) {
    if (-not (Test-Path $requiredPackage)) {
        if ($requiredPackage -like '*\winrt\base.h') {
            throw 'C++/WinRT headers not found. In the MSYS2 UCRT64 terminal, run: pacman -S --needed mingw-w64-ucrt-x86_64-cppwinrt'
        }
        throw "Required Qt package not found: $requiredPackage"
    }
}

$env:MOUFFETTE_QT_ROOT = $ucrtRoot
$env:Path = "$ucrtBin;$env:Path"

# Run the same CMake guardrail as tools/check_architecture_boundaries.sh.
# Calling CMake directly avoids a login shell resetting the UCRT64 PATH.
$guardScript = Join-Path $clientRoot 'tests\cmake\verify_qml_architecture.cmake'
& $cmake "-DSOURCE_ROOT=$clientRoot" -P $guardScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Architecture boundary checks failed.' }

$preset = if ($Configuration -eq 'Prod') { 'windows-release' } else { 'windows-debug' }
$label = if ($Configuration -eq 'Prod') { 'production (Release)' } else { 'development (Debug)' }
Write-Host "Building Mouffette for Windows $label..." -ForegroundColor Cyan

$configureArgs = @('--preset', $preset)
if ($Clean) { $configureArgs += '--fresh' }
if ($ConsoleLogs) { $configureArgs += '-DCONSOLE_OUTPUT=ON' }
& $cmake @configureArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

$buildArgs = @('--build', '--preset', $preset, '--parallel')
if ($Target) { $buildArgs += @('--target', $Target) }
& $cmake @buildArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

Write-Host "Build successful: out\build\$preset\Mouffette.exe" -ForegroundColor Green
Write-Host 'Use scripts\package-release.ps1 to create the standalone Release ZIP.' -ForegroundColor DarkGray
