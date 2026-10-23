#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-"$root/build-native"}
dist="$root/dist"
case "$(uname -s)" in
    Linux) platform=linux ;;
    Darwin) platform=macos ;;
    *) printf '%s\n' 'Run this script on Linux or macOS.' >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) arch=x64 ;;
    arm64|aarch64) arch=arm64 ;;
    *) printf '%s\n' 'Unsupported package architecture.' >&2; exit 1 ;;
esac
name="Spark-$platform-$arch"
mkdir -p "$dist"
stage=$(mktemp -d "$dist/.native-stage.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT HUP INT TERM
cmake --install "$build" --prefix "$stage/$name"
if [ "$platform" = macos ]; then
    licenses=${SPARK_DEPENDENCY_LICENSE_DIR:-"$(wx-config --prefix)/share/spark-dependencies"}
    for notice in WXWIDGETS_LICENSE WXWIDGETS_PREAMBLE WXWIDGETS_LGPL LIBPNG_LICENSE IJG_JPEG_README; do
        cp "$licenses/THIRD_PARTY_$notice.txt" "$stage/$name/Spark.app/Contents/Resources/" 2>/dev/null && continue
        printf '%s\n' "Missing macOS dependency notice: $licenses/THIRD_PARTY_$notice.txt" >&2
        exit 1
    done
    codesign --force --deep --sign - "$stage/$name/Spark.app"
else
    printf '%s\n' '#!/bin/sh' 'set -eu' \
        'root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)' \
        'exec "$root/bin/Spark" "$@"' > "$stage/$name/Spark"
    chmod +x "$stage/$name/Spark"
fi
if find "$stage/$name" \( -name config.json -o -name '*.jsonl' -o -name data -o -name workspace \) | grep .; then
    printf '%s\n' 'Refusing to package private state.' >&2; exit 1
fi
archive="$dist/$name.tar.gz"
tar -C "$stage" -czf "$archive.tmp" "$name"
mv -f "$archive.tmp" "$archive"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
else
    (cd "$dist" && shasum -a 256 "$name.tar.gz" > "$name.tar.gz.sha256")
fi
printf '%s\n' "$archive"
