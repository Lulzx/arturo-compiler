#!/bin/bash
# Install the pinned prebuilt Arturo host interpreter for CI and put it on PATH.
# The Linux build is dynamically linked against WebKitGTK, GTK, and MPFR, so it
# needs their runtime libraries (not the -dev packages, which pull in far more).
set -euo pipefail
version=${ARTURO_VERSION:-0.10.0}
os=$(uname -s); arch=$(uname -m)
case "$os" in
    Linux)
        sudo apt-get update -qq
        sudo apt-get install -y -qq --no-install-recommends \
            libwebkit2gtk-4.1-0 libgtk-3-0 libmpfr6 libgmp10 libssl-dev >/dev/null
        case "$arch" in
            aarch64|arm64) asset="arturo-$version-linux-arm64.zip" ;;
            *)             asset="arturo-$version-linux-amd64.zip" ;;
        esac ;;
    Darwin)
        case "$arch" in
            arm64) asset="arturo-$version-macos-arm64.zip" ;;
            *)     asset="arturo-$version-macos-amd64.zip" ;;
        esac ;;
    *) echo "unsupported OS: $os" >&2; exit 1 ;;
esac
dir=${1:-"$PWD/arturo-bin"}
mkdir -p "$dir"
curl -fsSL -o "$dir/arturo.zip" \
    "https://github.com/arturo-lang/arturo/releases/download/v$version/$asset"
unzip -qo "$dir/arturo.zip" -d "$dir"
chmod +x "$dir/arturo"
rm -f "$dir/arturo.zip"
if [ -n "${GITHUB_PATH:-}" ]; then echo "$dir" >> "$GITHUB_PATH"; fi
"$dir/arturo" --version | head -1
