# Third-Party Notices

## nlohmann/json

- Files: `third_party/nlohmann/json.hpp`
- License: MIT
- Copyright (c) 2013-2022 Niels Lohmann
- Full license: `third_party/nlohmann/LICENSE.MIT`

## ghc::filesystem

- Files: `third_party/ghc/filesystem.hpp`
- License: MIT
- Copyright (c) 2018, Steffen Schümann
- Full license: `third_party/ghc/LICENSE`
- Upstream: https://github.com/gulrak/filesystem
- Pinned release: `v1.5.16`
- Commit: `c306390e0d5670e3d832e7798d27e99e41668a9b`
- Unmodified source: `include/ghc/filesystem.hpp` and `LICENSE` from that commit.
- Header SHA-256: `b7a1c642416cbe10cf5c826ec59c65e2618e8a95d2507b08330dc5348a516757`
- License SHA-256: `1b265537c97cfb422ffb52492c96979943f54fab8746a6dd38d8329953d487cc`

The `SPARK_WINDOWS_XP` profile selects this implementation through
`src/platform/filesystem.h`; other profiles use `std::filesystem`. The upstream
release README documents Windows support without explicitly claiming Windows XP
support or listing XP in its tested configurations. Selecting this dependency
does not establish Windows XP runtime compatibility; that remains unverified.
Distributions that bundle this header must retain its MIT notice and license.

## md4c

- Files: `third_party/md4c/md4c.c`, `third_party/md4c/md4c.h`
- License: MIT
- Copyright and full license: `third_party/md4c/LICENSE.md`
- Vendored source information and checksums: `third_party/md4c/README.md`

Portable releases include the nlohmann/json and md4c license texts alongside the
executables.

## libcurl

Linux and macOS link against the operating system's libcurl. Windows 7 and
later use WinHTTP. The separate Windows XP build statically links curl 8.18.0
with Mbed TLS instead of the XP system TLS stack.

- Upstream: https://curl.se/
- XP release license: `THIRD_PARTY_CURL_LICENSE.txt`
- Official source archive SHA-256:
  `40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946`

## Mbed TLS (Windows XP Only)

The XP build links Mbed TLS 3.6.5 with Spark's XP entropy and mutex adapters.
TLS certificate and hostname verification remain enabled. `cacert.pem` is the
Mozilla CA certificate bundle distributed by curl; it must remain beside the
XP executables.

- Upstream: https://github.com/Mbed-TLS/mbedtls
- License: Apache-2.0 OR GPL-2.0-or-later; Spark uses Apache-2.0.
- XP release license: `THIRD_PARTY_MBEDTLS_LICENSE.txt`
- Official source archive SHA-256:
  `4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8`

## wxWidgets (Linux And macOS)

The native Linux/macOS GUI uses wxWidgets 3.2. Windows keeps the existing
Win32/GDI GUI. Linux packages use system wxWidgets libraries. A macOS package
that embeds wxWidgets must retain its wxWindows Library Licence and associated
notices, including licenses of any bundled image libraries.

- Upstream: https://www.wxwidgets.org/
- License: wxWindows Library Licence 3.1

The macOS dependency script also builds wxWidgets' bundled libpng and IJG
JPEG library. Their full notices are included in the app's Resources directory
as `THIRD_PARTY_LIBPNG_LICENSE.txt` and `THIRD_PARTY_IJG_JPEG_README.txt`.
This software is based in part on the work of the Independent JPEG Group.
