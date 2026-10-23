[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CurlRoot,
    [Parameter(Mandatory=$true)][string]$CertificateBundle,
    [string]$BuildDirectory = '',
    [string]$VisualStudioDirectory = '',
    [string]$CMakePath = 'cmake.exe',
    [string]$DumpbinPath = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $root 'build-xp-x86' }
if (-not (Test-Path -LiteralPath (Join-Path $CurlRoot 'include\curl\curl.h'))) {
    throw 'Supply an x86 XP-compatible libcurl SDK with asynchronous DNS and a modern TLS backend. The Windows system WinHTTP backend is not used for XP.'
}
if (-not (Test-Path -LiteralPath $CertificateBundle -PathType Leaf)) { throw 'A PEM CA certificate bundle is required.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install the Visual Studio 2017 v141_xp toolset and Windows 7.1A SDK first.' }
$instances = @(& $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.v141.x86.x64 `
    Microsoft.VisualStudio.Component.WinXP -format json | ConvertFrom-Json)
if ($VisualStudioDirectory) {
    $instance = $instances | Where-Object { $_.installationPath -eq $VisualStudioDirectory } | Select-Object -First 1
} else {
    $instance = $instances | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
}
if (-not $instance) { throw 'Install the v141 x86/x64 and Windows XP support components first.' }
$VisualStudioDirectory = $instance.installationPath
$major = ([version]$instance.installationVersion).Major
$generator = switch ($major) {
    15 { 'Visual Studio 15 2017' }
    16 { 'Visual Studio 16 2019' }
    17 { 'Visual Studio 17 2022' }
    18 { 'Visual Studio 18 2026' }
    default { throw "Unsupported Visual Studio generator version: $major" }
}
if (-not $DumpbinPath) {
    $compiler = Get-ChildItem -LiteralPath (Join-Path $VisualStudioDirectory 'VC\Tools\MSVC') -Directory |
        Where-Object { $_.Name -like '14.16.*' } | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $compiler) { throw 'MSVC 14.16 was not found in the selected Visual Studio installation.' }
    $DumpbinPath = Join-Path $compiler.FullName 'bin\Hostx86\x86\dumpbin.exe'
}
& $CMakePath -S $root -B $build -G $generator -A Win32 -T v141_xp `
    "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioDirectory" `
    '-DCMAKE_SYSTEM_VERSION=7.0' '-DSPARK_WINDOWS_XP=ON' '-DSPARK_BUILD_TESTS=OFF' `
    '-DSPARK_CORE_ONLY=OFF' '-DSPARK_PLATFORM_TESTS_ONLY=OFF' '-DSPARK_RUNTIME_TESTS_ONLY=OFF' `
    '-DSPARK_WIN7_COMPAT_PROBE=OFF' "-DCMAKE_PREFIX_PATH=$CurlRoot" '-DCURL_USE_STATIC_LIBS=ON'
if ($LASTEXITCODE -ne 0) { throw 'XP configuration failed.' }
& $CMakePath --build $build --config Release --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'XP build failed.' }
$stage = Join-Path $build 'xp-stage'
& $CMakePath --install $build --config Release --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'XP staging failed.' }
Copy-Item -LiteralPath $CertificateBundle -Destination (Join-Path $stage 'cacert.pem')
foreach ($library in @('CURL', 'MBEDTLS')) {
    Copy-Item -LiteralPath (Join-Path $CurlRoot ($library + '_LICENSE.txt')) `
        -Destination (Join-Path $stage ('THIRD_PARTY_' + $library + '_LICENSE.txt'))
}
# Audit dependency DLLs as well as the two executables before any XP run.
$forbidden = 'GetTickCount64|GetFinalPathNameByHandleW|InitializeCriticalSectionEx|SetProcessDPIAware|GetUserDefaultLocaleName|LCIDToLocaleName|GetFileInformationByHandleEx|CancelIoEx|BCrypt|api-ms-win-|ucrtbase\.dll'
Get-ChildItem -LiteralPath $stage -File | Where-Object { $_.Extension -in @('.exe','.dll') } | ForEach-Object {
    $imports = & $DumpbinPath /imports $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Import audit failed: $($_.Name)" }
    if ($imports | Select-String -Pattern $forbidden) { throw "Non-XP imports found: $($_.Name)" }
}
Write-Host "XP candidate staged at $stage. Run it in XP before distributing an XP release."
