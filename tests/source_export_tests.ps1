[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $root ('build-source-export-tests-' + [Guid]::NewGuid().ToString('N'))
$source = Join-Path $fixture 'dist\Spark-source'

function Assert-True($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-FixtureGit([string[]]$Arguments) {
    $output = & git -C $source @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Fixture Git failed: $output" }
    return $output
}

try {
    if (Test-Path -LiteralPath (Join-Path $root '.git')) {
        $trackedIgnored = @(& git -C $root ls-files --cached --ignored --exclude-standard)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot audit tracked source paths.' }
        Assert-True ($trackedIgnored.Count -eq 0) 'The repository tracks ignored private or generated files.'
    }
    New-Item -ItemType Directory -Path $fixture | Out-Null
    foreach ($entry in @('.github', 'cmake', 'resources', 'scripts', 'src', 'tests', 'third_party',
        '.gitignore', 'CMakeLists.txt', 'config.example.json', 'CONFIGURATION.md',
        'README.md', 'README.zh-CN.md', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $root $entry) -Destination $fixture -Recurse
    }
    foreach ($relative in @('AGENTS.md', 'AGENTS.override.md', 'src\AGENTS.md',
        'config.json', '.env', '.codex\settings.json', '.spark\state.json', 'data\session.jsonl',
        'workspace\private.txt', 'tmp\audit.txt')) {
        $path = Join-Path $fixture $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        [IO.File]::WriteAllText($path, 'Private fixture: must not be published.')
    }
    $obsolete = Join-Path $fixture 'src\export_obsolete.cpp'
    [IO.File]::WriteAllText($obsolete, '// Generated fixture.')
    $exporter = Join-Path $fixture 'scripts\export-source.ps1'
    $first = & $exporter
    $null = Invoke-FixtureGit @('init', '-q', '-b', 'main')
    $null = Invoke-FixtureGit @('config', 'user.name', 'Spark export test')
    $null = Invoke-FixtureGit @('config', 'user.email', 'spark-tests@example.invalid')
    $null = Invoke-FixtureGit @('config', 'commit.gpgsign', 'false')
    $null = Invoke-FixtureGit @('add', '.')
    $null = Invoke-FixtureGit @('commit', '-q', '-m', 'Fixture')
    $localGuide = Join-Path $source 'AGENTS.md'
    [IO.File]::WriteAllText($localGuide, 'Keep local guidance, never publish it.')
    $ignoredPaths = @('AGENTS.md', 'src/AGENTS.md', 'AGENTS.override.md',
        '.codex/settings.json', '.spark/state.json', 'tmp/audit.txt', 'config.json',
        'data/session.jsonl', 'workspace/private.txt', '.env')
    $ignored = @(Invoke-FixtureGit (@('check-ignore') + $ignoredPaths))
    foreach ($relative in $ignoredPaths) {
        Assert-True ($relative -in $ignored) "Private path is not ignored: $relative"
    }
    $null = Invoke-FixtureGit @('add', '-f', 'AGENTS.md')
    $null = Invoke-FixtureGit @('commit', '-q', '-m', 'Accidental internal guidance')
    $rejected = $false
    try { $null = & $exporter -Force }
    catch { $rejected = $_.Exception.Message -match 'internal guidance or private files' }
    Assert-True $rejected 'Export accepted tracked internal guidance.'
    Assert-True ([IO.File]::ReadAllText($localGuide) -eq 'Keep local guidance, never publish it.') `
        'Rejected export changed local guidance.'
    $null = Invoke-FixtureGit @('rm', '--cached', 'AGENTS.md')
    $null = Invoke-FixtureGit @('commit', '-q', '-m', 'Untrack internal guidance')
    $head = Invoke-FixtureGit @('rev-parse', 'HEAD')
    $marker = Join-Path $source '.git\private-marker'
    [IO.File]::WriteAllText($marker, 'Keep repository metadata.')
    $extra = Join-Path $source 'CONTRIBUTING.md'
    [IO.File]::WriteAllText($extra, 'Keep independently maintained files.')
    $readme = Join-Path $fixture 'README.md'
    [IO.File]::AppendAllText($readme, "`nUpdated fixture.`n")
    Remove-Item -LiteralPath $obsolete

    $second = & $exporter -Force
    Assert-True (Test-Path -LiteralPath $marker) 'Source export deleted Git metadata.'
    Assert-True ((Invoke-FixtureGit @('rev-parse', 'HEAD')) -eq $head) 'Source export changed history.'
    Assert-True ([IO.File]::ReadAllText($extra) -eq 'Keep independently maintained files.') `
        'Source export deleted an independently maintained file.'
    Assert-True ([IO.File]::ReadAllText($localGuide) -eq 'Keep local guidance, never publish it.') `
        'Source export deleted ignored local guidance.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $source 'src\export_obsolete.cpp'))) `
        'An obsolete exported file was retained.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $source 'README.md')).EndsWith("Updated fixture.`n")) `
        'The source repository was not updated.'
    $archive = [IO.Compression.ZipFile]::OpenRead($second.Zip)
    try {
        Assert-True (-not ($archive.Entries.FullName -match '(^|[\\/])\.git([\\/]|$)')) `
            'Git metadata leaked into the source archive.'
        Assert-True ($archive.Entries.FullName -contains '.gitignore') 'The archive omitted .gitignore.'
        Assert-True (-not ($archive.Entries.FullName -match `
            '(^|[\\/])(AGENTS(\.override)?\.md|config\.json|\.env|\.codex|\.spark|data|workspace|tmp)([\\/]|$)')) `
            'Internal guidance or private fixture data leaked into the archive.'
    } finally { $archive.Dispose() }

    $changedReadme = Join-Path $source 'README.md'
    [IO.File]::AppendAllText($changedReadme, 'Uncommitted user edit.')
    $before = (Get-FileHash -LiteralPath $changedReadme -Algorithm SHA256).Hash
    $rejected = $false
    try { $null = & $exporter -Force }
    catch { $rejected = $_.Exception.Message -match 'uncommitted' }
    Assert-True $rejected 'Export did not reject uncommitted tracked changes.'
    Assert-True ((Get-FileHash -LiteralPath $changedReadme -Algorithm SHA256).Hash -eq $before) `
        'A rejected export changed the working tree.'
    Write-Host 'PASS: source export excludes private fixtures, rejects tracked guidance, and preserves Git history, local files and uncommitted edits.'
} finally {
    $prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $checked = [IO.Path]::GetFullPath($fixture)
    if (-not $checked.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove fixtures outside the project.'
    }
    if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force }
}
