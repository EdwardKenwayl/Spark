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
    New-Item -ItemType Directory -Path $fixture | Out-Null
    foreach ($entry in @('.github', 'cmake', 'resources', 'scripts', 'src', 'tests', 'third_party',
        '.gitignore', 'CMakeLists.txt', 'config.example.json', 'CONFIGURATION.md',
        'README.md', 'README.zh-CN.md', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $root $entry) -Destination $fixture -Recurse
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
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $source 'src\export_obsolete.cpp'))) `
        'An obsolete exported file was retained.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $source 'README.md')).EndsWith("Updated fixture.`n")) `
        'The source repository was not updated.'
    $archive = [IO.Compression.ZipFile]::OpenRead($second.Zip)
    try {
        Assert-True (-not ($archive.Entries.FullName -match '(^|[\\/])\.git([\\/]|$)')) `
            'Git metadata leaked into the source archive.'
        Assert-True ($archive.Entries.FullName -contains '.gitignore') 'The archive omitted .gitignore.'
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
    Write-Host 'PASS: source export preserves Git history, custom files and uncommitted edits; removes obsolete exports.'
} finally {
    $prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $checked = [IO.Path]::GetFullPath($fixture)
    if (-not $checked.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove fixtures outside the project.'
    }
    if (Test-Path -LiteralPath $checked) { Remove-Item -LiteralPath $checked -Recurse -Force }
}
