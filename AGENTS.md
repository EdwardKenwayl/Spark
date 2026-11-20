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

## Cross-platform maintenance

- Shared core and transcript changes apply to Windows, Linux, and macOS.
  Check both the Windows chat view and the native GUI when rendering changes;
  they have separate replay and turn-folding entry points.
- Rebuild and update the existing Windows x64, Windows x86, Windows XP x86,
  Linux x64, and macOS x64 packages after shared fixes. Reuse the existing
  build scripts and VM transfer routes; preserve user configuration and data.
- Report source synchronization, successful builds, tests, package replacement,
  and guest runtime checks separately for each platform. If a toolchain or VM
  prevents completion, state the exact blocker; never describe an old package
  as updated or a build-only result as a guest runtime test.
