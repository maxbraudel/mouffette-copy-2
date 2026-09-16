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

# Qt deployment does not own direct libav* dependencies. Walk PE imports from
# the complete clean stage and copy the matching UCRT64 runtime recursively.
$runtimeBin = Join-Path $msysRoot 'ucrt64\bin'
$objdump = Join-Path $runtimeBin 'objdump.exe'
if (-not (Test-Path $objdump)) { throw "objdump is required for runtime deployment: $objdump" }
$stagedBin = Split-Path -Parent $clientExe
$pending = [System.Collections.Generic.Queue[string]]::new()
$visited = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem $stageDir -Recurse -File | Where-Object { $_.Extension -in '.dll', '.exe' } |
    ForEach-Object { $pending.Enqueue($_.FullName) }
while ($pending.Count -gt 0) {
    $binary = $pending.Dequeue()
    if (-not $visited.Add($binary)) { continue }
    $imports = & $objdump -p $binary 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect runtime dependencies of $binary" }
    foreach ($line in $imports) {
        if ($line -notmatch 'DLL Name:\s*(\S+)') { continue }
        $name = $Matches[1]
        $destination = Join-Path $stagedBin $name
        $source = Join-Path $runtimeBin $name
        if (Test-Path $destination) { $pending.Enqueue($destination); continue }
        if (Test-Path $source) {
            Copy-Item $source $destination
            $pending.Enqueue($destination)
            continue
        }
        if ($name -match '^(api-ms-|ext-ms-)' -or (Test-Path (Join-Path $env:SystemRoot "System32\$name"))) { continue }
        throw "Unresolved runtime dependency $name imported by $binary"
    }
}
foreach ($library in @('avformat', 'avcodec', 'avutil', 'swscale', 'swresample')) {
    if (-not (Get-ChildItem $stagedBin -Filter "$library-*.dll")) {
        throw "Packaged application is missing the resident decoder dependency $library"
    }
}

$webpPlugin = Join-Path (Split-Path -Parent $clientExe) 'imageformats\qwebp.dll'
if (-not (Test-Path $webpPlugin)) {
    throw "Packaged application is missing the required Qt WebP plugin: $webpPlugin"
}

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
