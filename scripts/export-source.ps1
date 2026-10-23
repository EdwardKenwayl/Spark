[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dist = Join-Path $root 'dist'
$source = Join-Path $dist 'Spark-source'
$stage = Join-Path $dist ('.source-stage-' + [Guid]::NewGuid().ToString('N'))
$zip = Join-Path $dist 'Spark-source.zip'
$temporaryZip = Join-Path $dist ('.source-' + [Guid]::NewGuid().ToString('N') + '.zip')
$prefix = $dist.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
foreach ($path in @($source, $stage, $zip, $temporaryZip)) {
    if (-not [IO.Path]::GetFullPath($path).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A source export path is outside dist.'
    }
}
if ((Test-Path -LiteralPath $source) -and -not $Force) {
    throw 'The source export already exists. Use -Force to replace this generated export.'
}
$sourceIsRepository = Test-Path -LiteralPath (Join-Path $source '.git')
$previousFiles = @()
if ($sourceIsRepository) {
    $status = & git -C $source status --porcelain --untracked-files=no
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the source repository.' }
    if ($status) { throw 'The source repository has uncommitted changes; commit or preserve them before exporting.' }
    if (Test-Path -LiteralPath $zip) {
        $previousArchive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $previousFiles = @($previousArchive.Entries | Where-Object { $_.Name } |
                ForEach-Object { $_.FullName.Replace('\', '/') })
        } finally { $previousArchive.Dispose() }
    }
}

$files = @(
    '.gitignore', 'CMakeLists.txt', 'config.example.json', 'README.md', 'README.zh-CN.md',
    'CONFIGURATION.md', 'THIRD_PARTY_NOTICES.md',
    'resources/spark_agent.ico', 'resources/spark_agent.icns', 'resources/spark_agent.svg', 'resources/spark_agent.png', 'resources/spark.desktop',
    'resources/spark_agent.rc', 'resources/spark_probe.rc',
    'resources/localization.rc',
    'scripts/build-win7.ps1', 'scripts/package-portable.ps1', 'scripts/export-source.ps1',
    'scripts/build-platform-tests.sh', 'scripts/build-runtime-tests.sh',
    'scripts/build-native.sh', 'scripts/package-native.sh', 'scripts/build-macos-deps.sh',
    'scripts/build-xp.ps1', 'scripts/build-xp-tls.ps1',
    'cmake/locale_resources.h.in', 'cmake/Info.plist.in',
    'cmake/xp-tls/CMakeLists.txt', 'cmake/xp-tls/spark_xp_tls_config.h', 'cmake/xp-tls/threading_alt.h',
    'tests/package_tests.ps1', 'tests/source_export_tests.ps1',
    'tests/tls_rejection_test.py',
    'third_party/nlohmann/json.hpp', 'third_party/nlohmann/LICENSE.MIT',
    'third_party/md4c/md4c.c', 'third_party/md4c/md4c.h',
    'third_party/md4c/LICENSE.md', 'third_party/md4c/README.md',
    'third_party/ghc/filesystem.hpp', 'third_party/ghc/LICENSE'
)
$files += @(Get-ChildItem -LiteralPath (Join-Path $root 'resources\locales') -File -Filter '*.json' |
    ForEach-Object { $_.FullName.Substring($root.Length + 1) })
foreach ($directory in @('src', 'tests')) {
    $entries = @(Get-ChildItem -LiteralPath (Join-Path $root $directory) -Recurse -Force)
    if ($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw "Source directory contains a link: $directory"
    }
    $files += @($entries | Where-Object { -not $_.PSIsContainer -and $_.Extension -in @('.cpp', '.h', '.mm') } |
        ForEach-Object { $_.FullName.Substring($root.Length + 1) })
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    foreach ($file in $files) {
        $original = Join-Path $root $file
        $destination = Join-Path $stage $file
        New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
        Copy-Item -LiteralPath $original -Destination $destination
    }
    $forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File -Force | Where-Object {
        $_.Name -eq 'config.json' -or $_.Extension -in @('.log', '.exe', '.pdb', '.obj', '.dmp', '.zip', '.rar', '.jsonl') -or
        $_.FullName.Substring($stage.Length) -match '[\\/](data|workspace|build[^\\/]*|recovery[^\\/]*)[\\/]'
    }
    if ($forbidden) { throw 'Unexpected private data or build outputs in the source export.' }
    $exportedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force)
    foreach ($file in $exportedFiles) {
        $relative = $file.FullName.Substring($stage.Length + 1)
        if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash) {
            throw "Source export differs from the project: $relative"
        }
    }
    # ZipFile includes .gitignore; Compress-Archive skips hidden files.
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $archive = [IO.Compression.ZipFile]::OpenRead($temporaryZip)
    try {
        if ($archive.Entries.Count -ne $exportedFiles.Count -or
            -not ($archive.Entries.FullName -contains '.gitignore')) {
            throw 'Source archive is incomplete.'
        }
    } finally { $archive.Dispose() }

    $checkedSource = [IO.Path]::GetFullPath($source)
    if (-not $checkedSource.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to replace a source export outside dist.'
    }
    if ($sourceIsRepository) {
        $sourcePrefix = $checkedSource.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        $currentFiles = @($files | ForEach-Object { $_.Replace('\', '/') })
        # Only remove paths owned by the preceding export, never .git or custom files.
        foreach ($relative in $previousFiles) {
            if ($relative -in $currentFiles) { continue }
            $obsolete = [IO.Path]::GetFullPath((Join-Path $checkedSource $relative))
            if (-not $obsolete.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                $relative -match '(^|/)\.git(/|$)') {
                throw 'An obsolete export path is outside the managed source tree.'
            }
            if (Test-Path -LiteralPath $obsolete -PathType Leaf) {
                Remove-Item -LiteralPath $obsolete
            }
        }
        foreach ($file in $files) {
            $destination = Join-Path $checkedSource $file
            New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
            Copy-Item -LiteralPath (Join-Path $stage $file) -Destination $destination -Force
        }
    } else {
        if (Test-Path -LiteralPath $checkedSource) { Remove-Item -LiteralPath $checkedSource -Recurse -Force }
        Move-Item -LiteralPath $stage -Destination $checkedSource
    }
    if (Test-Path -LiteralPath $zip) {
        [IO.File]::Replace($temporaryZip, $zip, [System.Management.Automation.Language.NullString]::Value)
    } else {
        Move-Item -LiteralPath $temporaryZip -Destination $zip
    }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(($zip + '.sha256'), "$hash  $(Split-Path $zip -Leaf)`r`n", [Text.Encoding]::ASCII)
    [pscustomobject]@{ Directory = $source; Zip = $zip; SHA256 = $hash; SourceFiles = $exportedFiles.Count }
} finally {
    $checkedStage = [IO.Path]::GetFullPath($stage)
    if (-not $checkedStage.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a temporary directory outside dist.'
    }
    if (Test-Path -LiteralPath $checkedStage) { Remove-Item -LiteralPath $checkedStage -Recurse -Force }
    if (Test-Path -LiteralPath $temporaryZip) { Remove-Item -LiteralPath $temporaryZip -Force }
}
