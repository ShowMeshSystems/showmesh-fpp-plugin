#!/usr/bin/env bash
# Confirms each release tarball's binary really is for the architecture
# its filename names. sha256sum -c only proves a tarball matches what this
# run just wrote; a build-path bug or a parallel-make race can still
# produce a tarball whose sha256 checks out for the wrong CPU.
set -euo pipefail

dist="${1:?usage: verify-artifact-arch.sh <dist-dir> <version>}"
version="${2:?missing version}"

check_one() {
    local arch="$1" pattern="$2"
    local path="$dist/showmesh-fpp-plugin_${version}_linux_${arch}.tar.gz"
    [ -f "$path" ] || { echo "verify-artifact-arch: missing $path" >&2; exit 1; }

    local workdir
    workdir=$(mktemp -d)
    tar -xzf "$path" -C "$workdir" showmesh-fpp-plugin

    local described
    described=$(file -b "$workdir/showmesh-fpp-plugin")
    rm -rf "$workdir"

    case "$described" in
        *"$pattern"*)
            echo "verify-artifact-arch: $arch OK ($described)"
            ;;
        *)
            echo "verify-artifact-arch: $path's binary is '$described', expected to contain '$pattern' for $arch" >&2
            exit 1
            ;;
    esac
}

check_one amd64 "x86-64"
check_one arm64 "ARM aarch64"
check_one armv7 "ARM, EABI5"
