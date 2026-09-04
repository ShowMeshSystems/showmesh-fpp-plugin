#!/usr/bin/env bash
# Builds libshowmesh-fpp10.so for one architecture inside a container based
# on debian:trixie -- the same base FalconChristmas/fpp's own Docker
# Dockerfile uses at the pinned FPP 10 commit -- via docker buildx, so
# arm64 and armv7 run under QEMU emulation and amd64 runs native. Writes the
# object plus a build-inputs sidecar into --out-dir. See
# native/adapters/prebuilt/Dockerfile for the actual compile step, which is
# the same `make -C native/adapters fpp10 FPP_SRC=...` the on-host install
# path runs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FPP_TAG="10.0"
FPP_COMMIT="370e62ed7e8c8318da6ee5b01312b8b75082d952"

# Single source of truth for the compile flags: baked into the Docker image
# build here, and recorded verbatim into the build-inputs sidecar below, so
# the two can never say something different about what was actually passed.
PREBUILT_CXXFLAGS="-std=c++20 -O2 -fPIC -Wall -Wextra -Werror -Wl,--build-id=none -ffile-prefix-map=/opt/showmesh-native=. -ffile-prefix-map=/opt/fpp=fpp-src"

ARCH=""
OUT_DIR="$REPO_ROOT/dist/prebuilt/fpp10"
VERIFY_REPRODUCIBLE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") --arch amd64|arm64|armv7 [--out-dir DIR] [--verify-reproducible]

  --arch ARCH             Target architecture. Required.
  --out-dir DIR            Where to write the object and its sidecar.
                            Default: dist/prebuilt/fpp10.
  --verify-reproducible    Build twice into DIR/.repro-a and DIR/.repro-b,
                            cmp the two objects, fail loudly on any
                            difference. Proves only that two builds run back
                            to back on this one machine, in this one docker
                            buildx instance, produce identical bytes -- not
                            that a different machine or docker version would.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --verify-reproducible) VERIFY_REPRODUCIBLE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$ARCH" in
    amd64) PLATFORM="linux/amd64" ;;
    arm64) PLATFORM="linux/arm64" ;;
    armv7) PLATFORM="linux/arm/v7" ;;
    *) echo "build-prebuilt-fpp10: --arch must be amd64, arm64 or armv7, got '$ARCH'" >&2; exit 2 ;;
esac

for dep in docker git; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        echo "build-prebuilt-fpp10: required dependency '$dep' is not on PATH" >&2
        exit 1
    fi
done

if [ -n "$(git -C "$REPO_ROOT" status --porcelain -- native)" ]; then
    echo "build-prebuilt-fpp10: refusing to build a prebuilt object from a dirty native/ tree" >&2
    git -C "$REPO_ROOT" status --porcelain -- native >&2
    exit 1
fi

REPO_COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD)"
# Pinned to this repository's own commit timestamp, the same pattern the
# root Makefile's release build uses for DIST_COMMIT_DATE, so two builds of
# one commit agree without depending on wall-clock time.
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" show -s --format=%ct HEAD)"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# Builds one object into $1 (an output directory), tagging the intermediate
# image with $2 so --verify-reproducible's two builds never share a cached
# container between the `docker create` extraction steps below.
build_once() {
    local out="$1" build_tag="$2"
    mkdir -p "$out"

    # --pull: docker's local image store keys FROM debian:trixie by name,
    # not by platform, so a single-platform image already cached under that
    # name (from an unrelated build) is reused as-is even when --platform
    # asks for a different one, with only a buildx warning, no failure. A
    # local debian:trixie cached for the wrong platform was observed to
    # produce a wrong-architecture object this way; --pull forces the
    # manifest-list resolution that picks the correct platform's image.
    docker buildx build \
        --platform "$PLATFORM" \
        -f "$REPO_ROOT/native/adapters/prebuilt/Dockerfile" \
        --build-arg "FPP_TAG=$FPP_TAG" \
        --build-arg "FPP_COMMIT=$FPP_COMMIT" \
        --build-arg "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
        --build-arg "PREBUILT_CXXFLAGS=$PREBUILT_CXXFLAGS" \
        --no-cache \
        --pull \
        -t "$build_tag" \
        --load \
        "$REPO_ROOT"

    local cid
    cid="$(docker create "$build_tag")"
    docker cp "$cid:/opt/showmesh-native/adapters/build/fpp10/libshowmesh-fpp10.so" "$out/libshowmesh-fpp10-${ARCH}.so"
    docker cp "$cid:/opt/build-compiler.txt" "$out/.compiler-${ARCH}.txt"
    docker rm "$cid" >/dev/null
    docker image rm "$build_tag" >/dev/null 2>&1 || true

    # The single hazard --pull does not fully close by itself: confirm the
    # object's own ELF header, not just the image's platform label, since
    # that is the thing that actually determines whether fppd can load it.
    local file_desc want_substr
    file_desc="$(file "$out/libshowmesh-fpp10-${ARCH}.so")"
    case "$ARCH" in
        amd64) want_substr="x86-64" ;;
        arm64) want_substr="ARM aarch64" ;;
        armv7) want_substr="ARM, EABI5" ;;
    esac
    if ! echo "$file_desc" | grep -q "$want_substr"; then
        echo "build-prebuilt-fpp10: FATAL: the object built for --arch $ARCH does not look like $want_substr: $file_desc" >&2
        exit 1
    fi
    echo "build-prebuilt-fpp10: confirmed $out/libshowmesh-fpp10-${ARCH}.so is $file_desc"

    local compiler_line
    compiler_line="$(head -1 "$out/.compiler-${ARCH}.txt" | sed 's/"/\\"/g')"
    local sha
    sha="$(sha256_of "$out/libshowmesh-fpp10-${ARCH}.so")"

    cat > "$out/libshowmesh-fpp10-${ARCH}.so.build.json" <<JSON
{
  "artifact": "libshowmesh-fpp10-${ARCH}.so",
  "architecture": "${ARCH}",
  "fppTag": "${FPP_TAG}",
  "fppCommit": "${FPP_COMMIT}",
  "repoCommit": "${REPO_COMMIT}",
  "containerBase": "debian:trixie",
  "containerPlatform": "${PLATFORM}",
  "compiler": "${compiler_line}",
  "compileFlags": "${PREBUILT_CXXFLAGS}",
  "sourceDateEpoch": ${SOURCE_DATE_EPOCH},
  "sha256": "${sha}"
}
JSON
    rm -f "$out/.compiler-${ARCH}.txt"
    echo "build-prebuilt-fpp10: wrote $out/libshowmesh-fpp10-${ARCH}.so (sha256 $sha) and its build.json sidecar"
}

if [ "$VERIFY_REPRODUCIBLE" = "1" ]; then
    a_dir="$OUT_DIR/.repro-a"
    b_dir="$OUT_DIR/.repro-b"
    rm -rf "$a_dir" "$b_dir"
    build_once "$a_dir" "showmesh-prebuilt-fpp10-verify-a:${ARCH}"
    build_once "$b_dir" "showmesh-prebuilt-fpp10-verify-b:${ARCH}"
    if ! cmp -s "$a_dir/libshowmesh-fpp10-${ARCH}.so" "$b_dir/libshowmesh-fpp10-${ARCH}.so"; then
        echo "build-prebuilt-fpp10: two independent builds of the same commit produced DIFFERENT ${ARCH} objects; the prebuilt object is not reproducible" >&2
        exit 1
    fi
    rm -rf "$a_dir" "$b_dir"
    echo "build-prebuilt-fpp10: verify-reproducible OK for ${ARCH}, two independent builds produced byte-identical objects"
    exit 0
fi

build_once "$OUT_DIR" "showmesh-prebuilt-fpp10:${ARCH}"
