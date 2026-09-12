param(
    [switch]$SkipTests,
    [switch]$RequireSigning
)

$ErrorActionPreference = 'Stop'
$scriptsRoot = $PSScriptRoot
$clientRoot = [IO.Path]::GetFullPath((Join-Path $scriptsRoot '..'))
Set-Location $clientRoot

if ($RequireSigning -and -not $env:MOUFFETTE_WINDOWS_CERTIFICATE) {
    throw 'MOUFFETTE_WINDOWS_CERTIFICATE is required for a signed public release.'
}

& (Join-Path $scriptsRoot 'build-release.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

if (-not $SkipTests) {
    Write-Host 'Running Release tests...' -ForegroundColor Cyan
    & ctest --preset windows-release --parallel | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Release tests failed.' }
}

$buildDir = Join-Path $clientRoot 'out\build\windows-release'
$stageDir = Join-Path $clientRoot 'out\stage\windows-release'
$packageDir = Join-Path $clientRoot 'out\packages'
$expectedStageRoot = [IO.Path]::GetFullPath((Join-Path $clientRoot 'out\stage'))
$resolvedStage = [IO.Path]::GetFullPath($stageDir)
if (-not $resolvedStage.StartsWith($expectedStageRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe staging path: $resolvedStage"
}

if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Write-Host 'Installing and deploying Qt runtime dependencies...' -ForegroundColor Cyan
& cmake --install $buildDir --prefix $stageDir | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake install/deployment failed.' }

$clientExe = Join-Path $stageDir 'bin\Mouffette.exe'
if (-not (Test-Path $clientExe)) { throw "Installed application not found: $clientExe" }

# Re-run the matching Qt deployment tool with QML source scanning enabled.
$msysRoot = if ($env:MOUFFETTE_MSYS2_ROOT) { $env:MOUFFETTE_MSYS2_ROOT } else { 'C:\msys64' }
$windeployqt = Join-Path $msysRoot 'ucrt64\bin\windeployqt6.exe'
if (-not (Test-Path $windeployqt)) { throw "windeployqt6 not found: $windeployqt" }
& $windeployqt --release --no-translations --qmldir (Join-Path $clientRoot 'resources\qml') `
    --dir (Split-Path -Parent $clientExe) $clientExe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'windeployqt QML deployment failed.' }

if ($env:MOUFFETTE_WINDOWS_CERTIFICATE) {
    $signTool = (Get-Command signtool.exe -ErrorAction SilentlyContinue).Source
    if (-not $signTool) { throw 'signtool.exe was not found in PATH.' }
    $signArgs = @('sign', '/fd', 'SHA256', '/td', 'SHA256', '/tr', 'http://timestamp.digicert.com',
                  '/f', $env:MOUFFETTE_WINDOWS_CERTIFICATE)
    if ($env:MOUFFETTE_WINDOWS_CERTIFICATE_PASSWORD) {
        $signArgs += @('/p', $env:MOUFFETTE_WINDOWS_CERTIFICATE_PASSWORD)
    }
    $signArgs += $clientExe
    & $signTool @signArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Authenticode signing failed.' }
    & $signTool verify /pa $clientExe | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Authenticode verification failed.' }
} else {
    Write-Warning 'No signing certificate configured; producing an unsigned local-test ZIP.'
}

$versionLine = (Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') `
    -Pattern '^MouffetteClient_VERSION:STATIC=(.+)$' | Select-Object -First 1)
$version = if ($versionLine) { $versionLine.Matches[0].Groups[1].Value } else { '1.0.0' }
$archive = Join-Path $packageDir "Mouffette-$version-windows-x64.zip"
if (Test-Path $archive) { Remove-Item $archive -Force }
if (Test-Path "$archive.sha256") { Remove-Item "$archive.sha256" -Force }

Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -Path $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path "$archive.sha256" -Value "$hash  $([IO.Path]::GetFileName($archive))" -Encoding ascii

Write-Host "Production package created: $archive" -ForegroundColor Green
Write-Host "Checksum: $archive.sha256" -ForegroundColor Green
