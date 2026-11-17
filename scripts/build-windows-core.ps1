param(
    [ValidateSet("amd64", "arm64")]
    [string]$Architecture = "amd64",
    [Parameter(Mandatory)]
    [string]$OutputDir,
    [string]$VersionResource,
    [switch]$TestDeviceManagement
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$output = (Resolve-Path -LiteralPath $OutputDir).Path
$go = (Get-Command go.exe).Source
$upstream = "github.com/mythologyli/zju-connect@v1.3.1"
$moduleJson = & $go mod download -json $upstream
if ($LASTEXITCODE -ne 0) {
    throw "Could not retrieve the pinned zju-connect source from the Go module cache or proxy"
}
$module = ($moduleJson -join "`n") | ConvertFrom-Json
if ($module.Sum -cne "h1:04aln3/Y+hG2vkKTe6nC+hbXJ/9ewHG1xZRTWtlBCXU=") {
    throw "Unexpected zju-connect source checksum"
}

$stagingRoot = (New-Item -ItemType Directory -Path (Join-Path $repoRoot "build/windows-core-work") -Force).FullName
$work = (New-Item -ItemType Directory -Path (Join-Path $stagingRoot ([guid]::NewGuid().ToString("N")))).FullName
$previous = @{
    GOOS = $env:GOOS
    GOARCH = $env:GOARCH
    CGO_ENABLED = $env:CGO_ENABLED
    GIT_CEILING_DIRECTORIES = $env:GIT_CEILING_DIRECTORIES
}
try {
    Expand-Archive -LiteralPath $module.Zip -DestinationPath $work
    $source = Join-Path $work $upstream
    $patch = Join-Path $PSScriptRoot "windows-core/zju-connect-v1.3.1-tun.patch"
    $env:GIT_CEILING_DIRECTORIES = $work
    & git.exe -C $source apply --whitespace=error $patch
    if ($LASTEXITCODE -ne 0) {
        throw "Could not apply the Windows TUN patch to zju-connect v1.3.1"
    }
    & git.exe -C $source apply --whitespace=error (Join-Path $PSScriptRoot "windows-core/zju-connect-v1.3.1-devices.patch")
    if ($LASTEXITCODE -ne 0) { throw "Could not apply the aTrust device-management patch" }
    & git.exe -C $source apply --whitespace=error (Join-Path $PSScriptRoot "windows-core/zju-connect-v1.3.1-dns.patch")
    if ($LASTEXITCODE -ne 0) { throw "Could not apply the DNS interception switch patch" }
    & git.exe -C $source apply --whitespace=error (Join-Path $PSScriptRoot "windows-core/zju-connect-v1.3.1-identity.patch")
    if ($LASTEXITCODE -ne 0) { throw "Could not apply the Windows device identity patch" }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_identity_windows.go') -Destination (Join-Path $source 'client/atrust/auth/ez4_device_identity_windows.go')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_identity_other.go') -Destination (Join-Path $source 'client/atrust/auth/ez4_device_identity_other.go')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_management.go') -Destination (Join-Path $source 'client/atrust/ez4_device_management.go')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_list.go') -Destination (Join-Path $source 'client/atrust/auth/ez4_device_list.go')
    if ($TestDeviceManagement) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_identity_test.go') -Destination (Join-Path $source 'client/atrust/auth/ez4_device_identity_windows_test.go')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/dns_switch_test.go') -Destination (Join-Path $source 'stack/tun/ez4_dns_switch_test.go')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_management_test.go') -Destination (Join-Path $source 'client/atrust/ez4_device_management_test.go')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-core/device_list_test.go') -Destination (Join-Path $source 'client/atrust/auth/ez4_device_list_test.go')
    }

    $env:GOOS = "windows"
    $env:GOARCH = $Architecture
    $env:CGO_ENABLED = "0"
    if ($VersionResource) {
        $target = if ($Architecture -eq 'amd64') { 'pe-x86-64' } else { 'pe-aarch64' }
        & windres.exe --input-format=rc --output-format=coff --target=$target $VersionResource (Join-Path $source 'ez4connectpro.syso')
        if ($LASTEXITCODE -ne 0) { throw 'Could not compile the VPN core process description' }
    }
    Push-Location $source
    try {
        if ($TestDeviceManagement) {
            & $go test -mod=readonly -count=1 -run '^TestEZ4|^Test.*Tunnel' ./client/atrust ./client/atrust/auth ./stack/tun
            if ($LASTEXITCODE -ne 0) { throw 'aTrust device management tests failed' }
        }
        & $go build -mod=readonly -trimpath -buildvcs=false -ldflags "-s -w -X main.zjuConnectVersion=v1.3.1 -X main.CommitID=ez4tun1" `
            -o (Join-Path $output "zju-connect.exe") .
        if ($LASTEXITCODE -ne 0) {
            throw "Could not build the Windows zju-connect core"
        }
    } finally {
        Pop-Location
    }
    Copy-Item -LiteralPath (Join-Path $source "LICENSE") -Destination (Join-Path $output "LICENSE-zju-connect.txt")
} finally {
    foreach ($key in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($key, $previous[$key])
    }
    $resolvedWork = (Resolve-Path -LiteralPath $work).Path
    $resolvedRoot = (Resolve-Path -LiteralPath $stagingRoot).Path.TrimEnd('\', '/')
    if ($resolvedWork.StartsWith($resolvedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
