[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$CMakePath = '',
    [string]$NinjaPath = '',
    [string]$VisualStudioDirectory = '',
    [string]$BuildDirectory = '',
    [ValidateRange(1, 16)]
    [int]$ParallelJobs = 1,
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$defaultBuild = if ($Architecture -eq 'x64') { 'build-win7' } else { 'build-win7-x86' }
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $root $defaultBuild }

if (-not $VisualStudioDirectory) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Visual Studio Installer was not found. Supply -VisualStudioDirectory explicitly.'
    }
    $VisualStudioDirectory = (& $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1)
}
if (-not $VisualStudioDirectory) { throw 'Install Visual Studio C++ tools, including MSVC 14.38.' }
$environmentScript = if ($Architecture -eq 'x64') { 'vcvars64.bat' } else { 'vcvarsamd64_x86.bat' }
$vcvars = Join-Path $VisualStudioDirectory ('VC\Auxiliary\Build\' + $environmentScript)
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Compiler environment was not found: $vcvars" }

if (-not $CMakePath) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $CMakePath = if ($cmakeCommand) { $cmakeCommand.Source } else {
        Join-Path $VisualStudioDirectory 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
}
$cmake = [IO.Path]::GetFullPath($CMakePath)
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake was not found: $cmake" }
if (-not (Test-Path -LiteralPath $ctest)) { throw "CTest was not found: $ctest" }
if (-not $NinjaPath) {
    $ninjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
    $NinjaPath = if ($ninjaCommand) { $ninjaCommand.Source } else {
        Join-Path $VisualStudioDirectory 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    }
}
$ninja = [IO.Path]::GetFullPath($NinjaPath)
if (-not (Test-Path -LiteralPath $ninja)) { throw "Ninja was not found: $ninja. Supply -NinjaPath." }

# The installed MSVC toolchain emits localized /showIncludes text even with
# VSLANG=1033, which Ninja cannot always decode into reliable header dependencies.
# Keep a clean rebuild so stale objects can never cross configuration changes.
foreach ($ninja_state in @('.ninja_deps', '.ninja_log')) {
    $state_path = Join-Path $build $ninja_state
    if (Test-Path -LiteralPath $state_path) { Remove-Item -LiteralPath $state_path -Force }
}

$configure = 'call "{0}" -vcvars_ver=14.38 >nul && "{1}" -S "{2}" -B "{3}" -G Ninja "-DCMAKE_MAKE_PROGRAM={5}" -DCMAKE_BUILD_TYPE={4} -DSPARK_BUILD_TESTS=ON -DSPARK_WIN7_COMPAT_PROBE=ON -DSPARK_PLATFORM_TESTS_ONLY=OFF -DSPARK_RUNTIME_TESTS_ONLY=OFF -DSPARK_CORE_ONLY=OFF -DSPARK_WINDOWS_XP=OFF' -f $vcvars, $cmake, $root, $build, $Configuration, $ninja
cmd.exe /d /s /c $configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

$compile = 'call "{0}" -vcvars_ver=14.38 >nul && "{1}" --build "{2}" --config {3} --clean-first --parallel {4}' -f $vcvars, $cmake, $build, $Configuration, $ParallelJobs
cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$test = 'call "{0}" -vcvars_ver=14.38 >nul && "{1}" --test-dir "{2}" --output-on-failure --timeout 30' -f $vcvars, $ctest, $build
cmd.exe /d /s /c $test
if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE" }

$binaries = @('Spark.exe', 'SparkMCP.exe')
foreach ($binary in $binaries) {
    $imports = Join-Path $build ($binary + '.imports.txt')
    $audit = 'call "{0}" -vcvars_ver=14.38 >nul && dumpbin /imports "{1}\{2}" > "{3}"' -f $vcvars, $build, $binary, $imports
    cmd.exe /d /s /c $audit
    if ($LASTEXITCODE -ne 0) { throw "PE import audit failed for ${binary} with exit code $LASTEXITCODE" }

    $forbidden = Select-String -Path $imports -Pattern @(
        'GetSystemTimePreciseAsFileTime',
        'SetThreadDescription',
        'GetDpiForWindow',
        'api-ms-win-crt',
        'api-ms-win-core'
    ) -ErrorAction SilentlyContinue
    if ($forbidden) {
        throw "Forbidden Windows 8+ or UCRT imports were found in ${binary}:`n$($forbidden.Line -join "`n")"
    }
}

Write-Host "Win7 $Architecture build passed: $build\Spark.exe and $build\SparkMCP.exe"
