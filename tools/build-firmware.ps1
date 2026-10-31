param(
    [ValidateSet('build', 'upload', 'monitor')][string]$Action = 'build',
    [string]$Port = ''
)
$ErrorActionPreference = 'Stop'
$repositoryPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$firmwarePath = Join-Path $repositoryPath 'firmware'
$projectPath = $firmwarePath
if ($Action -eq 'monitor') {
    $pioArguments = @('device', 'monitor', '-b', '115200', '--dtr', '0', '--rts', '0', '--eol', 'LF', '--filter', 'send_on_enter')
    if ($Port) { $pioArguments += @('-p', $Port) }
    & pio @pioArguments
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO failed with exit code $LASTEXITCODE" }
    exit 0
}
# CMake expands Windows 8.3 paths back to long names. Stage build inputs when
# necessary; the user's source directory remains in its original location.
if ($repositoryPath.Contains(' ')) {
    $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
    try { $hash = $hashAlgorithm.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($repositoryPath)) }
    finally { $hashAlgorithm.Dispose() }
    $suffix = ([BitConverter]::ToString($hash)).Replace('-', '').Substring(0, 12)
    $stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) "iris-udp-$suffix"
    if ($stagingRoot.Contains(' ')) { throw 'TEMP contains spaces. Set TEMP to a path without spaces for this build.' }
    $projectPath = Join-Path $stagingRoot 'firmware'
    New-Item -ItemType Directory -Force -Path $projectPath | Out-Null
    foreach ($directoryName in @('src', 'include')) {
        $destination = Join-Path $projectPath $directoryName
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Get-ChildItem -LiteralPath (Join-Path $firmwarePath $directoryName) |
            Copy-Item -Destination $destination -Recurse -Force
    }
    $commonDestination = Join-Path $stagingRoot 'common'
    New-Item -ItemType Directory -Force -Path $commonDestination | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $repositoryPath 'common') |
        Copy-Item -Destination $commonDestination -Recurse -Force
    foreach ($fileName in @('platformio.ini', 'platformio.local.ini', 'CMakeLists.txt', 'sdkconfig.defaults', 'partitions.csv', 'dependencies.lock', 'pio_compat.py', 'wifi_config.py', 'wifi.ini')) {
        $sourceFile = Join-Path $firmwarePath $fileName
        if (Test-Path -LiteralPath $sourceFile) { Copy-Item -LiteralPath $sourceFile -Destination $projectPath -Force }
        elseif ($fileName -eq 'wifi.ini') {
            # Overwrite stale staged credentials if the source INI was removed.
            Copy-Item -LiteralPath (Join-Path $firmwarePath 'wifi.example.ini') -Destination (Join-Path $projectPath 'wifi.ini') -Force
        }
    }
    Write-Host "Build directory: $projectPath"
}
$pioArguments = @('run', '-d', $projectPath, '-e', 'xiao_esp32s3_ov2640')
if ($Action -eq 'upload') {
    $pioArguments += @('-t', 'upload')
    if ($Port) { $pioArguments += @('--upload-port', $Port) }
}
& pio @pioArguments
if ($LASTEXITCODE -ne 0) { throw "PlatformIO failed with exit code $LASTEXITCODE" }
$outputPath = Join-Path $firmwarePath 'artifacts'
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$binaryPath = Join-Path $projectPath '.pio/build/xiao_esp32s3_ov2640'
foreach ($artifact in @('firmware.bin', 'firmware.elf', 'bootloader.bin', 'partitions.bin')) {
    Copy-Item -LiteralPath (Join-Path $binaryPath $artifact) -Destination $outputPath -Force
}
Set-Content -LiteralPath (Join-Path $outputPath 'build-directory.txt') -Value $projectPath
if ($projectPath -ne $firmwarePath) {
    Copy-Item -LiteralPath (Join-Path $projectPath 'dependencies.lock') -Destination $firmwarePath -Force
}
Write-Host "Firmware artifacts: $outputPath"
