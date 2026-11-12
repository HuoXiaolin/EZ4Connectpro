param(
    [string]$BuildDir = "build/windows-x64",
    [string]$ToolsRoot,
    [string]$SdkRoot,
    [string]$GoModules,
    [switch]$Test,
    [switch]$Package
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $ToolsRoot) {
    $workRoot = Split-Path -Parent $repoRoot
    if ((Split-Path -Leaf $workRoot) -eq "Github_Source") { $workRoot = Split-Path -Parent $workRoot }
    $ToolsRoot = Join-Path $workRoot "toolchain"
}
if (-not $SdkRoot) {
    $SdkRoot = Join-Path $ToolsRoot "EZ4Connectpro/sdk"
    if (-not (Test-Path -LiteralPath $SdkRoot -PathType Container)) { $SdkRoot = Join-Path $repoRoot "build/sdk" }
}
if (-not $GoModules) {
    $GoModules = Join-Path $ToolsRoot "EZ4Connectpro/go-mod-cache"
    if (-not (Test-Path -LiteralPath $GoModules -PathType Container)) { $GoModules = Join-Path $repoRoot "build/go-mod-cache" }
}
$qt = Join-Path $SdkRoot "qt"
$compiler = Join-Path $SdkRoot "Tools/mingw1310_64/bin"
$go = Join-Path $SdkRoot "go/bin"
$cmake = Join-Path $ToolsRoot "cmake/cmake-3.31.6-windows-x86_64/bin/cmake.exe"
$ninja = Join-Path $ToolsRoot "mingw/mingw64/bin/ninja.exe"
foreach ($file in @($cmake, $ninja, "$qt/bin/windeployqt.exe", "$compiler/g++.exe", "$go/go.exe")) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required existing tool is missing: $file" }
}
if ($Package -and -not (Test-Path -LiteralPath $GoModules -PathType Container)) {
    throw "Existing Go module dependencies are missing: $GoModules"
}
$previous = @{}
foreach ($name in @('PATH', 'GOTOOLCHAIN', 'GOPROXY', 'GOSUMDB', 'GOMODCACHE', 'GOCACHE', 'GOPATH')) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name)
}
Push-Location $repoRoot
try {
    $env:PATH = "$qt/bin;$compiler;$go;$env:PATH"
    $env:GOTOOLCHAIN = "local"
    $env:GOPROXY = "off"
    $env:GOSUMDB = "off"
    $env:GOMODCACHE = $GoModules
    $env:GOCACHE = Join-Path $repoRoot "build/go-cache"
    $env:GOPATH = Join-Path $repoRoot "build/go-path"
    & $cmake -S . -B $BuildDir -G Ninja '-UREL_VER' '-DCMAKE_BUILD_TYPE=Release' '-DBUILD_TESTING=ON' `
        "-DCMAKE_PREFIX_PATH=$qt" "-DCMAKE_C_COMPILER=$compiler/gcc.exe" `
        "-DCMAKE_CXX_COMPILER=$compiler/g++.exe" "-DCMAKE_MAKE_PROGRAM=$ninja"
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
    & $cmake --build $BuildDir --parallel 8
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed" }
    if ($Test) {
        $runner = Join-Path $BuildDir "windows_desktop_check.exe"
        $ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
        & $runner $ctest "--test-dir `"$BuildDir`" --output-on-failure -j 8" --no-window
        if ($LASTEXITCODE -ne 0) { throw "Independent desktop tests failed; no foreground fallback was attempted" }
        $goTests = (New-Item -ItemType Directory -Path (Join-Path $repoRoot 'build/windows-portable-tests') -Force).FullName
        Copy-Item -Path (Join-Path $PSScriptRoot 'windows-portable/*.go') -Destination $goTests
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows-portable/go.mod') -Destination $goTests
        [IO.File]::WriteAllBytes((Join-Path $goTests 'payload.zip'), [byte[]]@(80,75,5,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0))
        Push-Location $goTests
        try {
            & go.exe test -count=1 .
            if ($LASTEXITCODE -ne 0) { throw 'Portable runtime cleanup safety tests failed' }
        } finally { Pop-Location }
    }
    if ($Package) {
        & (Join-Path $PSScriptRoot "package-windows-single-file.ps1") -BuildDir $BuildDir
    }
} finally {
    Pop-Location
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name]) }
}
