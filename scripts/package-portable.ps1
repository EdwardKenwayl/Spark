[CmdletBinding()]
param(
    [string]$OutputDirectory = '',
    [string]$BuildDirectory = '',
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',
    [switch]$WindowsXP
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$defaultBuild = if ($Architecture -eq 'x64') { 'build-win7' } else { 'build-win7-x86' }
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $root $defaultBuild }
$dist = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'dist' }
$prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $dist.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must be inside the project: $dist"
}

$packageName = if ($WindowsXP) { 'Spark-windows-xp-x86' } else { 'Spark-windows-' + $Architecture }
if ($WindowsXP -and $Architecture -ne 'x86') { throw 'The XP package requires -Architecture x86.' }
$legacyLive = Join-Path $dist ('Spark-portable-win7-' + $Architecture)
$live = Join-Path $dist $packageName
# Preserve installed MCP paths and private state when updating an older layout.
if (-not $WindowsXP -and -not (Test-Path -LiteralPath $live) -and (Test-Path -LiteralPath $legacyLive -PathType Container)) {
    $live = $legacyLive
}
$stage = Join-Path $dist ('.portable-stage-' + [Guid]::NewGuid().ToString('N'))
$zip = Join-Path $dist ($packageName + '.zip')
$temporaryZip = Join-Path $dist ('.portable-' + [Guid]::NewGuid().ToString('N') + '.zip')
$binaries = @('Spark.exe', 'SparkMCP.exe')
$additionalFiles = if ($WindowsXP) {
    @('cacert.pem', 'THIRD_PARTY_GHC_FILESYSTEM_LICENSE.txt',
      'THIRD_PARTY_CURL_LICENSE.txt', 'THIRD_PARTY_MBEDTLS_LICENSE.txt')
} else { @() }
$documents = @('README.md', 'README.zh-CN.md', 'CONFIGURATION.md', 'THIRD_PARTY_NOTICES.md')
$obsoleteDocuments = @(
    'DESIGN.md', 'COMPACTION_TESTS.md', 'CODEX_GAP_AUDIT.md', 'CODEX_PORT_PLAN.md',
    'IMPLEMENTATION_SPEC.md', 'LICENSE', 'NOTICE'
)
foreach ($binary in $binaries) {
    $path = Join-Path $build $binary
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing build output: $binary" }
    $reader = New-Object IO.BinaryReader([IO.File]::OpenRead($path))
    try {
        if ($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) {
            throw "Invalid PE executable: $binary"
        }
        $reader.BaseStream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 64 -or $peOffset -gt ($reader.BaseStream.Length - 6)) {
            throw "Invalid PE header offset: $binary"
        }
        $reader.BaseStream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $binary" }
        $machine = $reader.ReadUInt16()
        $expectedMachine = if ($Architecture -eq 'x64') { 0x8664 } else { 0x14c }
        if ($machine -ne $expectedMachine) {
            throw "PE machine architecture mismatch for ${binary}: expected $Architecture, found 0x$($machine.ToString('x4'))."
        }
    } finally { $reader.Dispose() }
}
foreach ($document in $documents + @('config.example.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $document))) { throw "Missing release file: $document" }
}
foreach ($file in $additionalFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $file) -PathType Leaf)) {
        throw "Missing XP release file: $file"
    }
}

function Get-PrivateStateHashes {
    $files = @()
    foreach ($name in @('config.json', 'data', 'workspace')) {
        $path = Join-Path $live $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $files += Get-Item -LiteralPath $path
        } else {
            $files += Get-ChildItem -LiteralPath $path -Recurse -File -Force
        }
    }
    @($files | Sort-Object FullName | ForEach-Object {
        $_.FullName + '|' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    })
}

$privateBefore = @(Get-PrivateStateHashes)
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    foreach ($binary in $binaries) { Copy-Item -LiteralPath (Join-Path $build $binary) -Destination $stage }
    foreach ($file in $additionalFiles) { Copy-Item -LiteralPath (Join-Path $build $file) -Destination $stage }
    foreach ($document in $documents) { Copy-Item -LiteralPath (Join-Path $root $document) -Destination $stage }
    $reference = [IO.File]::ReadAllText((Join-Path $root 'CONFIGURATION.md'))
    $reference = ($reference -replace "`r`n", "`n") -replace "`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $stage 'CONFIGURATION.md'), $reference, (New-Object Text.UTF8Encoding($true)))
    $example = [IO.File]::ReadAllText((Join-Path $root 'config.example.json'))
    if ($example -notmatch '"path"\s*:\s*"\$\{DOCUMENTS\}/Spark"' -or
        $example -notmatch '"data_dir"\s*:\s*"\./') {
        throw 'The example configuration must use portable workspace and data paths.'
    }
    # Keep the commented example readable in Windows 7 Notepad.
    $example = ($example -replace "`r`n", "`n") -replace "`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $stage 'config.example.json'), $example, (New-Object Text.UTF8Encoding($true)))
    Copy-Item -LiteralPath (Join-Path $root 'third_party\nlohmann\LICENSE.MIT') `
        -Destination (Join-Path $stage 'THIRD_PARTY_NLOHMANN_JSON_LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\md4c\LICENSE.md') `
        -Destination (Join-Path $stage 'THIRD_PARTY_MD4C_LICENSE.txt')

    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $archive = [IO.Compression.ZipFile]::OpenRead($temporaryZip)
    try {
        if ($archive.Entries.Count -ne ($binaries.Count + $additionalFiles.Count + $documents.Count + 3)) {
            throw 'Unexpected release archive contents.'
        }
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName -match '(^|/)(config\.json|data|workspace)(/|$)') {
                throw 'Private state was included in the release archive.'
            }
        }
    } finally { $archive.Dispose() }

    New-Item -ItemType Directory -Force -Path $live | Out-Null
    # Update only the release allowlist. Never move, delete or rewrite private state.
    foreach ($file in (Get-ChildItem -LiteralPath $stage -File)) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $live $file.Name) -Force
    }
    foreach ($document in $obsoleteDocuments) {
        $path = Join-Path $live $document
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    $privateAfter = @(Get-PrivateStateHashes)
    if (Compare-Object $privateBefore $privateAfter) {
        throw 'Private state changed during packaging. Check whether an agent is still running.'
    }
    if (Test-Path -LiteralPath $zip) {
        [IO.File]::Replace($temporaryZip, $zip, [System.Management.Automation.Language.NullString]::Value)
    } else {
        Move-Item -LiteralPath $temporaryZip -Destination $zip
    }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(($zip + '.sha256'), "$hash  $(Split-Path $zip -Leaf)`r`n", [Text.Encoding]::ASCII)
    foreach ($oldArchive in $(if ($WindowsXP) { @() } else { @(($legacyLive + '.zip'), ($legacyLive + '.zip.sha256')) })) {
        if (Test-Path -LiteralPath $oldArchive -PathType Leaf) {
            Remove-Item -LiteralPath $oldArchive -Force
        }
    }
    [pscustomobject]@{
        Directory = $live
        Zip = $zip
        SHA256 = $hash
        ProtectedFilesVerified = $privateBefore.Count
    }
} finally {
    $checkedStage = [IO.Path]::GetFullPath($stage)
    if (-not $checkedStage.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a temporary directory outside the project.'
    }
    if (Test-Path -LiteralPath $checkedStage) { Remove-Item -LiteralPath $checkedStage -Recurse -Force }
    if (Test-Path -LiteralPath $temporaryZip) { Remove-Item -LiteralPath $temporaryZip -Force }
}
