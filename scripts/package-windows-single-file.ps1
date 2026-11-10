param(
    [string]$BuildDir = "build",
    [string]$CoreArchive,
    [string]$OutputFile = "out/EZ4Connectpro-windows-x64.exe"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputFile)
$resource = Join-Path $build "EZ4Connectpro.rc"
if (-not (Test-Path -LiteralPath $resource -PathType Leaf)) {
    throw "Windows file information is missing. Reconfigure and build EZ4Connectpro before packaging."
}
$windres = Get-Command windres.exe
if ($CoreArchive) {
    $CoreArchive = (Resolve-Path -LiteralPath $CoreArchive).Path
}

$stagingRoot = (New-Item -ItemType Directory -Path (Join-Path $repoRoot "build/portable-work") -Force).FullName
$work = (New-Item -ItemType Directory -Path (Join-Path $stagingRoot ([guid]::NewGuid().ToString("N")))).FullName
$runtime = Join-Path $work "runtime"
try {
    & (Join-Path $PSScriptRoot "deploy-windows.ps1") -BuildDir $build -Architecture amd64 `
        -OutputDir $runtime -CoreArchive $CoreArchive -Portable

    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $runtime "LICENSE-EZ4Connect.txt")
    $files = Get-ChildItem -LiteralPath $runtime -File -Recurse
    if ($files.Name -match "WebEngine|Qt6Qml|Qt6Quick") {
        throw "Unexpected browser or QML dependency in the portable payload"
    }

    Compress-Archive -Path (Join-Path $runtime "*") -DestinationPath (Join-Path $work "payload.zip") -CompressionLevel Optimal
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "windows-portable/main.go") -Destination $work
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "windows-portable/go.mod") -Destination $work

    & $windres.Source --input-format=rc --output-format=coff --target=pe-x86-64 $resource (Join-Path $work "launcher.syso")
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to compile the portable launcher icon and file information"
    }

    Push-Location $work
    $previousGo = @{ GOOS = $env:GOOS; GOARCH = $env:GOARCH; CGO_ENABLED = $env:CGO_ENABLED }
    try {
        $env:GOOS = "windows"
        $env:GOARCH = "amd64"
        $env:CGO_ENABLED = "0"
        & go.exe build -trimpath -buildvcs=false -ldflags "-s -w -H=windowsgui" -o "portable.exe" .
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to compile the Windows x64 portable launcher"
        }
    } finally {
        foreach ($key in $previousGo.Keys) {
            [Environment]::SetEnvironmentVariable($key, $previousGo[$key])
        }
        Pop-Location
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $work "portable.exe") -Destination $output -Force
    Get-Item -LiteralPath $output | Select-Object FullName, Length
    Write-Output ("Runtime payload before compression: {0:N0} bytes" -f (($files | Measure-Object -Property Length -Sum).Sum))
} finally {
    $resolvedWork = (Resolve-Path -LiteralPath $work).Path
    $resolvedRoot = (Resolve-Path -LiteralPath $stagingRoot).Path.TrimEnd('\', '/')
    if ($resolvedWork.StartsWith($resolvedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
