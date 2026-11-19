# Spark Development Workflow

## Windows 7 x64 build and package

- Build from this directory with
  `.\scripts\build-win7.ps1 -Architecture x64 -ParallelJobs 2`.
- Run `.\scripts\package-portable.ps1 -Architecture x64` after the build.
- The package script updates the existing Win7 x64 application directory,
  creates/replaces `dist\Spark-windows-x64.zip`, and writes its SHA-256 file.
- The current application directory may retain its legacy name
  `dist\Spark-portable-win7-x64`. Inspect the script's selected output rather
  than renaming or creating a second install directory.

## Protect local state

- Packaging must preserve the destination's `config.json`, `data`, and
  `workspace`. The package script hashes these paths before and after updating
  the executable allowlist; do not bypass that check.
- Do not terminate a running Spark process to replace its executable. If the
  package step cannot replace a locked executable, report that and leave the
  running application and its data untouched.
- Do not include private configuration or conversation data in the ZIP.

## Source repository and GitHub

- The clean, public source repository is this checkout. Build and package from
  the maintained working source, then synchronize only the intended source
  and documentation changes into this repository.
- Run focused tests first, then the Win7 x64 build script (which runs the test
  suite and Windows 7 import audit).
- Review `git diff --check`, the staged diff, and `git status` before committing.
- Commit and push completed code fixes to `origin/main` when the user requests
  normal maintenance. Do not create or push tags and do not publish GitHub
  releases unless the user explicitly requests them.
