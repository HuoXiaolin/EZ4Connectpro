# Windows deployment script
# Run from the repository root with Qt and the compiler on PATH.

param(
    [string]$TargetName = "EZ4Connectpro",
    [string]$DisplayName = "EZ4Connectpro",
    [string]$BuildDir = "build",
    [ValidateSet("amd64", "arm64")]
    [string]$Architecture = "amd64",
    [string]$Nightly = "false",
    [string]$OutputDir = $DisplayName,
    [string]$CoreArchive,
    [switch]$Portable
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$app = Join-Path $BuildDir "Release/$TargetName.exe"
if (-not (Test-Path -LiteralPath $app -PathType Leaf)) {
    $app = Join-Path $BuildDir "$TargetName.exe"
}
$app = (Resolve-Path -LiteralPath $app).Path
if (Test-Path -LiteralPath $OutputDir) {
    throw "Deployment directory already exists: $OutputDir"
}
$output = (New-Item -ItemType Directory -Path $OutputDir).FullName
$executable = Join-Path $output "$TargetName.exe"
Copy-Item -LiteralPath $app -Destination $executable

$deployArgs = @("--release", "--dir", $output)
if ($Portable) {
    $deployArgs += @("--no-opengl-sw", "--no-system-d3d-compiler", "--no-system-dxc-compiler", "--translations", "zh_CN")
}
& windeployqt.exe @deployArgs $executable
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

$downloadedArchive = !$CoreArchive -and $Nightly -eq "true"
if ($downloadedArchive) {
    $url = "https://github.com/Mythologyli/zju-connect/releases/download/nightly/zju-connect-windows-$Architecture.zip"
    $CoreArchive = Join-Path $output "zju-connect-download.zip"
    Invoke-WebRequest -Uri $url -OutFile $CoreArchive
}
if ($CoreArchive) {
    try {
        Expand-Archive -LiteralPath $CoreArchive -DestinationPath $output -Force
    } finally {
        if ($downloadedArchive) {
            Remove-Item -LiteralPath $CoreArchive -Force
        }
    }
} else {
    & (Join-Path $PSScriptRoot "build-windows-core.ps1") -Architecture $Architecture -OutputDir $output
}
if (-not (Test-Path -LiteralPath (Join-Path $output "zju-connect.exe") -PathType Leaf)) {
    throw "The deployment does not contain zju-connect.exe"
}

Copy-Item -LiteralPath (Join-Path $repoRoot "libs/wintun/bin/$Architecture/wintun.dll") -Destination $output
Copy-Item -LiteralPath (Join-Path $repoRoot "resource/qt.conf") -Destination $output

$redistName = if ($Architecture -eq "amd64") { "vc_redist.x64.exe" } else { "vc_redist.arm64.exe" }
$redist = Join-Path $output $redistName
if (Test-Path -LiteralPath $redist -PathType Leaf) {
    Remove-Item -LiteralPath $redist
}
