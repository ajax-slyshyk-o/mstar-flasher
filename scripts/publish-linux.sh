#!/usr/bin/env bash
# Build/test a native Linux Release executable and create a local archive.
set -euo pipefail

usage() {
    cat <<'HELP'
Usage: publish-linux.sh [--output-dir DIR] [--jobs N]

Build the linux-release preset with FTDI and tests enabled, run offline tests,
then package mstar-cli, README.md and scripts/setup-linux.sh.

Options:
  --output-dir DIR  Archive destination (default: <repo>/build/publish).
                   Relative paths are resolved from the calling directory.
  --jobs N         Build parallelism (default: CMAKE_BUILD_PARALLEL_LEVEL or nproc).
  -h, --help       Show this help.

Requires the normal Linux build prerequisites, including VCPKG_ROOT.
Creates mstar-flasher-linux-<architecture>.tar.gz. Replaces an existing archive
only after successful packaging. System libraries are not bundled: the target
machine needs compatible libstdc++, libftdi1 and libusb runtime libraries.
Nothing is uploaded or installed, and no programmer is opened.
HELP
}

die() { printf 'publish-linux: %s\n' "$*" >&2; exit 1; }

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_dir="$repo_dir/build/publish"
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-}
while (($#)); do
    case "$1" in
        --output-dir)
            (($# >= 2)) && [[ -n $2 ]] || die '--output-dir needs a directory'
            output_dir=$2
            shift 2 ;;
        --jobs)
            (($# >= 2)) && [[ $2 =~ ^[1-9][0-9]*$ ]] || die '--jobs needs a positive integer'
            jobs=$2
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

[[ $(uname -s) == Linux ]] || die 'run this script on Linux'
for tool in cmake ctest tar gzip install mktemp nproc; do
    command -v "$tool" >/dev/null || die "required tool not found: $tool"
done
[[ -n ${VCPKG_ROOT:-} && -f $VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake ]] \
    || die 'set VCPKG_ROOT to your vcpkg checkout'
jobs=${jobs:-$(nproc)}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die 'build parallelism must be a positive integer'

# Resolve before changing directories, so --output-dir works from anywhere.
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd)
cd -- "$repo_dir"

cmake --preset linux-release -DCMAKE_BUILD_TYPE=Release \
    -DMSTAR_ENABLE_FTDI=ON -DMSTAR_BUILD_TESTS=ON
cmake --build --preset linux-release --parallel "$jobs"
ctest --preset linux-release --no-tests=error

stage_dir=$(mktemp -d "$output_dir/.publish-XXXXXX")
trap 'rm -rf -- "$stage_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

package="mstar-flasher-linux-$(uname -m)"
mkdir -p -- "$stage_dir/$package/scripts"
install -m 0755 build/linux-release/mstar-cli "$stage_dir/$package/mstar-cli"
install -m 0644 README.md "$stage_dir/$package/README.md"
install -m 0755 scripts/setup-linux.sh "$stage_dir/$package/scripts/setup-linux.sh"

tar --create --gzip --file "$stage_dir/$package.tar.gz" \
    --owner=0 --group=0 --numeric-owner --directory "$stage_dir" -- "$package"
mv -fT -- "$stage_dir/$package.tar.gz" "$output_dir/$package.tar.gz"
printf '\nCreated: %s\n' "$output_dir/$package.tar.gz"
