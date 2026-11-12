param([switch]$Apply)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$build = Join-Path $repo "build"
if (-not (Test-Path -LiteralPath $build)) { Write-Output 'No build cache to clean.'; return }
if ((Get-Item -LiteralPath $build).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'The build directory is a reparse point; nothing was removed.'
}
$known = @(
    'browser-smoke', 'browser-smoke-chrome', 'browser-smoke-default', 'browser-smoke-unrestricted', 'browser-smoke-zen',
    'desktop-qa', 'go-cache', 'go-path', 'multi-tun-core-work', 'portable-work', 'references', 'release-candidate',
    'taste', 'tray-integration', 'tray-verification', 'portable-final-check', 'windows-core-work',
    'windows-x64', 'windows-x64-final', 'windows-portable-tests', 'ez4connectpro-workspace-x64.exe', 'portable-candidate.exe'
)
$items = @(Get-ChildItem -LiteralPath $build -Force | Where-Object {
    $_.Name -in $known -or $_.Name -match '^portable-smoke-[0-9a-f]{32}$'
})
$plan = @()
foreach ($item in $items) {
    $path = (Resolve-Path -LiteralPath $item.FullName).Path
    if (-not $path.StartsWith($build + '\', [StringComparison]::OrdinalIgnoreCase) -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe cleanup target: $path" }
    $children = @(if ($item.PSIsContainer) { Get-ChildItem -LiteralPath $path -Force -Recurse })
    if ($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw "Reparse point below cleanup target: $path"
    }
    $files = @(if ($item.PSIsContainer) { $children | Where-Object { -not $_.PSIsContainer } } else { $item })
    $plan += [pscustomobject]@{ Path=$path; Files=$files; Bytes=($files | Measure-Object Length -Sum).Sum }
}
$plan | Select-Object Path, @{n='Files';e={$_.Files.Count}}, Bytes | Format-Table -AutoSize
Write-Output ('Cleanup bytes: ' + ($plan | Measure-Object Bytes -Sum).Sum)
if (-not $Apply) { Write-Output 'Preview only. Use -Apply after validating the latest delivery.'; return }

# Complete preflight before removing any entry. Do not stop other processes or follow links.
$runningPaths = @(Get-Process | ForEach-Object { try { $_.Path } catch {} } | Where-Object { $_ })
foreach ($entry in $plan) {
    if ($runningPaths | Where-Object { $_ -eq $entry.Path -or $_.StartsWith($entry.Path + '\', [StringComparison]::OrdinalIgnoreCase) }) {
        throw "A program is running from this cleanup target: $($entry.Path)"
    }
    foreach ($file in $entry.Files) {
        $stream = $null
        try { $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None) }
        finally { if ($stream) { $stream.Dispose() } }
    }
}
foreach ($entry in $plan) {
    Remove-Item -LiteralPath $entry.Path -Force -Recurse
}
if (-not (Get-ChildItem -LiteralPath $build -Force)) { Remove-Item -LiteralPath $build -Force }
Write-Output ('Removed bytes: ' + ($plan | Measure-Object Bytes -Sum).Sum)
