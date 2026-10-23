[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $root ('build-package-tests-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'scripts\package-portable.ps1'

function Assert-True($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Write-TestPE([string]$Path, [uint16]$Machine) {
    $stream = [IO.File]::Create($Path)
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $writer.Write([uint16]0x5a4d)
        $stream.Position = 0x3c
        $writer.Write([int32]0x80)
        $stream.Position = 0x80
        $writer.Write([uint32]0x4550)
        $writer.Write($Machine)
    } finally { $writer.Dispose() }
}

try {
    foreach ($architecture in @('x86', 'x64')) {
        $build = Join-Path $fixture ('build-' + $architecture)
        $dist = Join-Path $fixture ('dist-' + $architecture)
        $live = Join-Path $dist ('Spark-windows-' + $architecture)
        New-Item -ItemType Directory -Force -Path $build, $live | Out-Null
        foreach ($binary in @('Spark.exe', 'SparkMCP.exe')) {
            $machine = if ($architecture -eq 'x86') { 0x14c } else { 0x8664 }
            Write-TestPE (Join-Path $build $binary) $machine
        }
        foreach ($privateFile in @('config.json', 'data\session.json', 'workspace\work.txt')) {
            $path = Join-Path $live $privateFile
            New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
            [IO.File]::WriteAllText($path, 'private fixture - must stay unchanged')
        }
        $before = @(Get-ChildItem -LiteralPath $live -Recurse -File |
            Sort-Object FullName | Get-FileHash -Algorithm SHA256 |
            ForEach-Object { $_.Path + '|' + $_.Hash })
        $arguments = @{ BuildDirectory = $build; OutputDirectory = $dist }
        if ($architecture -eq 'x86') { $arguments.Architecture = $architecture }
        $result = & $package @arguments
        Assert-True ($result.Directory -eq $live) 'Wrong architecture output directory.'
        Assert-True ((Split-Path $result.Zip -Leaf) -eq ('Spark-windows-' + $architecture + '.zip')) `
            'Release names must identify Windows and architecture, not the minimum OS.'
        Assert-True ($result.ProtectedFilesVerified -eq 3) 'Private-state check was skipped.'
        $after = @(Get-ChildItem -LiteralPath $live -Recurse -File |
            Where-Object { $_.Name -in @('config.json', 'session.json', 'work.txt') } |
            Sort-Object FullName | Get-FileHash -Algorithm SHA256 |
            ForEach-Object { $_.Path + '|' + $_.Hash })
        Assert-True (-not (Compare-Object $before $after)) 'Private files changed.'
        $archive = [IO.Compression.ZipFile]::OpenRead($result.Zip)
        try {
            Assert-True ($archive.Entries.Count -eq 9) 'Unexpected archive contents.'
            Assert-True (-not ($archive.Entries.FullName -match '(^|/)(config\.json|data|workspace)(/|$)')) `
                'Private state leaked into the archive.'
        } finally { $archive.Dispose() }
        $hash = (Get-FileHash -LiteralPath $result.Zip -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-True ($hash -eq $result.SHA256) 'Wrong archive hash.'
        Assert-True ([IO.File]::ReadAllText($result.Zip + '.sha256').StartsWith($hash + '  ')) `
            'Wrong checksum file.'
        $second = & $package @arguments
        Assert-True ($second.ProtectedFilesVerified -eq 3) 'Update did not preserve private state.'
        if ($architecture -eq 'x64') {
            $positional = & $package $dist $build
            Assert-True ($positional.Directory -eq $live) 'Legacy positional arguments changed.'
        }

        $other = if ($architecture -eq 'x86') { 'x64' } else { 'x86' }
        $rejected = $false
        try { $null = & $package -Architecture $other -BuildDirectory $build -OutputDirectory $dist }
        catch { $rejected = $_.Exception.Message -match 'machine|architecture' }
        Assert-True $rejected 'Architecture mismatch was not rejected.'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $dist ('Spark-windows-' + $other)))) `
            'Wrong-architecture release was created.'

        $legacyDist = Join-Path $fixture ('legacy-' + $architecture)
        $legacyLive = Join-Path $legacyDist ('Spark-portable-win7-' + $architecture)
        New-Item -ItemType Directory -Force -Path $legacyLive | Out-Null
        $config = Join-Path $legacyLive 'config.json'
        [IO.File]::WriteAllText($config, 'preserve the configured MCP path and settings')
        $legacyZip = $legacyLive + '.zip'
        [IO.File]::WriteAllText($legacyZip, 'old package')
        [IO.File]::WriteAllText($legacyZip + '.sha256', 'old checksum')
        $legacyResult = & $package -Architecture $architecture -BuildDirectory $build -OutputDirectory $legacyDist
        Assert-True ($legacyResult.Directory -eq $legacyLive) 'Existing runtime directory was moved.'
        Assert-True ($legacyResult.ProtectedFilesVerified -eq 1) 'Legacy settings were not checked.'
        Assert-True ([IO.File]::ReadAllText($config) -eq 'preserve the configured MCP path and settings') `
            'Legacy settings changed.'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $legacyDist ('Spark-windows-' + $architecture)))) `
            'A duplicate runtime directory was created.'
        Assert-True (-not (Test-Path -LiteralPath $legacyZip)) 'Old ZIP was left beside the renamed package.'
        Assert-True (-not (Test-Path -LiteralPath ($legacyZip + '.sha256'))) 'Old checksum was left behind.'

        if ($architecture -eq 'x86') {
            foreach ($file in @('cacert.pem', 'THIRD_PARTY_GHC_FILESYSTEM_LICENSE.txt',
                'THIRD_PARTY_CURL_LICENSE.txt', 'THIRD_PARTY_MBEDTLS_LICENSE.txt')) {
                [IO.File]::WriteAllText((Join-Path $build $file), 'XP fixture dependency notice')
            }
            $xpResult = & $package -WindowsXP -Architecture x86 -BuildDirectory $build -OutputDirectory $legacyDist
            Assert-True ((Split-Path $xpResult.Zip -Leaf) -eq 'Spark-windows-xp-x86.zip') 'Wrong XP archive name.'
            Assert-True ($xpResult.Directory -ne $legacyLive) 'XP package reused the Win7 runtime directory.'
            Assert-True ([IO.File]::ReadAllText($config) -eq 'preserve the configured MCP path and settings') `
                'XP packaging changed the Win7 configuration.'
            $xpArchive = [IO.Compression.ZipFile]::OpenRead($xpResult.Zip)
            try {
                Assert-True ($xpArchive.Entries.Count -eq 13) 'XP dependencies are missing from the archive.'
                Assert-True ($xpArchive.Entries.FullName -contains 'cacert.pem') 'XP CA bundle is missing.'
            } finally { $xpArchive.Dispose() }
        }

        [IO.File]::WriteAllText((Join-Path $build 'Spark.exe'), 'not an executable')
        $rejected = $false
        try { $null = & $package @arguments }
        catch { $rejected = $_.Exception.Message -match 'PE|executable' }
        Assert-True $rejected 'Invalid executable was not rejected.'
        Write-Host "PASS: $architecture packaging, update, privacy, checksum and PE checks"
    }
} finally {
    $prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $checked = [IO.Path]::GetFullPath($fixture)
    if (-not $checked.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove fixtures outside the project.'
    }
    if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force }
}
