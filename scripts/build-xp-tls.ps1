[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$MbedTlsSource,
    [Parameter(Mandatory=$true)][string]$CurlSource,
    [Parameter(Mandatory=$true)][string]$InstallPrefix,
    [string]$BuildDirectory = '',
    [string]$VisualStudioDirectory = '',
    [string]$CMakePath = 'cmake.exe'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $root 'build-xp-tls' }
$MbedTlsSource = [IO.Path]::GetFullPath($MbedTlsSource)
$CurlSource = [IO.Path]::GetFullPath($CurlSource)
$InstallPrefix = [IO.Path]::GetFullPath($InstallPrefix)
if (-not (Test-Path -LiteralPath (Join-Path $MbedTlsSource 'include\mbedtls\build_info.h')) -or
    -not (Test-Path -LiteralPath (Join-Path $CurlSource 'include\curl\curl.h'))) {
    throw 'Supply the unpacked official Mbed TLS 3.6.5 and curl 8.18.0 release archives.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$instances = @(& $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.v141.x86.x64 `
    Microsoft.VisualStudio.Component.WinXP -format json | ConvertFrom-Json)
if ($VisualStudioDirectory) {
    $instance = $instances | Where-Object { $_.installationPath -eq $VisualStudioDirectory } | Select-Object -First 1
} else {
    $instance = $instances | Where-Object { ([version]$_.installationVersion).Major -eq 17 } | Select-Object -First 1
}
if (-not $instance) { throw 'Supply Visual Studio 2022 with the v141 and Windows XP support components.' }
& $CMakePath -S (Join-Path $root 'cmake\xp-tls') -B $build -G 'Visual Studio 17 2022' `
    -A Win32 -T v141_xp "-DCMAKE_GENERATOR_INSTANCE=$($instance.installationPath)" `
    '-DCMAKE_SYSTEM_VERSION=7.0' "-DCMAKE_INSTALL_PREFIX=$InstallPrefix" `
    "-DMBEDTLS_SOURCE=$MbedTlsSource" "-DCURL_SOURCE=$CurlSource"
if ($LASTEXITCODE -ne 0) { throw 'XP TLS dependency configuration failed.' }
& $CMakePath --build $build --config Release --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'XP TLS dependency build failed.' }
& $CMakePath --install $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'XP TLS dependency installation failed.' }
Copy-Item -LiteralPath (Join-Path $MbedTlsSource 'LICENSE') -Destination (Join-Path $InstallPrefix 'MBEDTLS_LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $CurlSource 'COPYING') -Destination (Join-Path $InstallPrefix 'CURL_LICENSE.txt')
Write-Host "XP TLS SDK installed at $InstallPrefix"
