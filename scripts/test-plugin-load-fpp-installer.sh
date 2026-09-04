#!/usr/bin/env bash
# Bench scaffolding, not the product. Proves that the PACKAGING repository's
# real installer (fpp-showmesh's scripts/fpp_install.sh, driving
# sm_install_native / sm_install_native_prebuilt) produces a loading plugin
# both on the prebuilt-fetch path and on the compile-fallback path, and that
# compiler invocation is positively observable in both directions.
#
# Unlike scripts/test-plugin-load-fpp.sh, this script never hand-installs the
# adapter itself. It stands up the same containerized fppd, then drives a
# read-only local copy of the fpp-showmesh installer inside that container
# exactly the way FPP itself would run it.
#
# --cpu is not exposed: every case here runs arm64 only, against the two
# correctly-tagged cached bench images (showmesh-bench/fpp:10.0-arm64 and
# showmesh-bench/fpp:9.5.3-arm64), because this Mac's Docker Desktop VM disk
# has too little free space to pull or build anything new. See README.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCH_DIR="$REPO_ROOT/bench/fpp-plugin-load"

BENCH_ID="${BENCH_ID:-sm517}"
BENCH_HTTP_PORT="${BENCH_HTTP_PORT:-8197}"
BENCH_FPP_MAJOR="${BENCH_FPP_MAJOR:-fpp10}"
# fpp10 only: whether the plugin tree installed into the container carries a
# fpp10-verified-versions.txt that lists this host's FPP version (10.0.0).
# "yes" drives the prebuilt-fetch path; "no" drives the compile fallback by
# making the running host's version look unverified to this install, the
# same way a real host running an FPP point release older than the plugin's
# verified list would.
BENCH_VERIFIED="${BENCH_VERIFIED:-yes}"
FPP_SHOWMESH_COMMIT="${FPP_SHOWMESH_COMMIT:-e96de471a6a429562af486297444525b2cd0f85e}"
DOWN=0
# Deliberate-failure demonstration only: forces the no_compiler_assertion to
# expect invoked=no regardless of --verified, so it can be shown FAILING
# against a run that actually takes the fallback (--verified no). Never used
# by the three real cases.
FORCE_EXPECT_NO_COMPILER=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  --id ID          Isolates this run. Default: \$BENCH_ID or "sm517".
  --port PORT      Host port for the container's HTTP API. Default:
                   \$BENCH_HTTP_PORT or 8197.
  --major MAJOR    fpp9 or fpp10. Default: \$BENCH_FPP_MAJOR or fpp10.
  --verified BOOL  yes or no (fpp10 only). yes drives the prebuilt-object
                   fetch path; no drives the compile fallback by shipping a
                   verified-versions list that does not name this host's FPP
                   version. Default: \$BENCH_VERIFIED or yes.
  --down           Tear this run down and exit.
  -h, --help       This message.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --id) BENCH_ID="$2"; shift 2 ;;
        --port) BENCH_HTTP_PORT="$2"; shift 2 ;;
        --major) BENCH_FPP_MAJOR="$2"; shift 2 ;;
        --verified) BENCH_VERIFIED="$2"; shift 2 ;;
        --down) DOWN=1; shift ;;
        --demo-force-expect-no-compiler) FORCE_EXPECT_NO_COMPILER=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for dep in docker curl jq git sha256sum; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        if [ "$dep" = "sha256sum" ] && command -v shasum >/dev/null 2>&1; then
            continue
        fi
        echo "test-plugin-load-fpp-installer: required dependency '$dep' is not on PATH" >&2
        exit 1
    fi
done

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

case "$BENCH_FPP_MAJOR" in
    fpp9)
        FPP_TAG="9.5.3"
        FPP_IMAGE="showmesh-bench/fpp:9.5.3-arm64"
        ;;
    fpp10)
        FPP_TAG="10.0"
        FPP_IMAGE="showmesh-bench/fpp:10.0-arm64"
        ;;
    *)
        echo "test-plugin-load-fpp-installer: --major must be fpp9 or fpp10, got '$BENCH_FPP_MAJOR'" >&2
        exit 2
        ;;
esac
FPP_PLATFORM="linux/arm64"

PROJECT="showmesh-fppbench-${BENCH_ID}"
CONTAINER="showmesh-fppbench-${BENCH_ID}-fpp"
COMPOSE=(docker compose -p "$PROJECT" -f "$BENCH_DIR/docker-compose.yml")

if [ "$DOWN" = "1" ]; then
    echo "test-plugin-load-fpp-installer: tearing down $PROJECT (including its named media volume)"
    FPP_IMAGE="$FPP_IMAGE" FPP_TAG="$FPP_TAG" FPP_COMMIT="unused" \
        FPP_BUILD_CONTEXT="$BENCH_DIR" FPP_PLATFORM="$FPP_PLATFORM" \
        BENCH_ID="$BENCH_ID" BENCH_HTTP_PORT="$BENCH_HTTP_PORT" \
        "${COMPOSE[@]}" down -v
    exit 0
fi

if ! docker image inspect "$FPP_IMAGE" >/dev/null 2>&1; then
    echo "test-plugin-load-fpp-installer: REFUSING to run: $FPP_IMAGE is not cached, and this script never pulls or builds an image (Docker Desktop VM disk constraint)" >&2
    exit 1
fi
cached_arch="$(docker image inspect "$FPP_IMAGE" --format '{{.Architecture}}')"
if [ "$cached_arch" != "arm64" ]; then
    echo "test-plugin-load-fpp-installer: REFUSING to run: $FPP_IMAGE is cached as architecture '$cached_arch', not arm64" >&2
    exit 1
fi

# The FPP source checkout under this image tag was already prepared by a
# prior scripts/test-plugin-load-fpp.sh run; compose still needs a build
# context value to interpolate even though FPP_IMAGE already exists and
# no build will happen.
FPP_BUILD_CONTEXT="$BENCH_DIR/.fpp-src/${BENCH_FPP_MAJOR}-${BENCH_ID}"
mkdir -p "$FPP_BUILD_CONTEXT"

export FPP_IMAGE FPP_TAG FPP_COMMIT="unused" FPP_BUILD_CONTEXT FPP_PLATFORM BENCH_ID BENCH_HTTP_PORT

echo "test-plugin-load-fpp-installer: FPP major=$BENCH_FPP_MAJOR tag=$FPP_TAG image=$FPP_IMAGE platform=$FPP_PLATFORM verified=$BENCH_VERIFIED"
echo "test-plugin-load-fpp-installer: docker compose up -d --force-recreate"
"${COMPOSE[@]}" up -d --force-recreate

wait_for_http() {
    local tries="${1:-90}" i=0
    while [ "$i" -lt "$tries" ]; do
        if curl -fsS -o /dev/null --max-time 2 "http://localhost:${BENCH_HTTP_PORT}/api/fppd/status"; then
            return 0
        fi
        i=$((i + 1))
        sleep 2
    done
    return 1
}

echo "test-plugin-load-fpp-installer: waiting for http://localhost:${BENCH_HTTP_PORT}/api/fppd/status"
if ! wait_for_http 90; then
    echo "test-plugin-load-fpp-installer: fppd did not answer within the wait budget" >&2
    docker logs "$CONTAINER" --tail 100 >&2 || true
    exit 1
fi

restart_and_wait() {
    "${COMPOSE[@]}" up -d --force-recreate >/dev/null
    if ! wait_for_http 90; then
        echo "test-plugin-load-fpp-installer: fppd did not come back up after restart" >&2
        docker logs "$CONTAINER" --tail 100 >&2 || true
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# A read-only local checkout of the PACKAGING repository (fpp-showmesh),
# pinned to the exact commit FPP_SHOWMESH_COMMIT (main at the time the
# installer this bench proves was merged). Never modified; this script only
# ever reads from it and copies it (with two bench-owned files swapped,
# below) into the container's plugin directory. Verified by commit, not by
# a moving branch or short ref, the same discipline
# scripts/test-plugin-load-fpp.sh applies to its own FPP checkout.
# ---------------------------------------------------------------------------

PKG_CHECKOUT="$BENCH_DIR/.fpp-showmesh-src/${BENCH_ID}"
if [ ! -d "$PKG_CHECKOUT/.git" ] || [ "$(git -C "$PKG_CHECKOUT" rev-parse HEAD 2>/dev/null)" != "$FPP_SHOWMESH_COMMIT" ]; then
    echo "test-plugin-load-fpp-installer: preparing a read-only local checkout of ShowMeshSystems/fpp-showmesh at $FPP_SHOWMESH_COMMIT"
    rm -rf "$PKG_CHECKOUT"
    mkdir -p "$PKG_CHECKOUT"
    git -C "$PKG_CHECKOUT" init -q
    git -C "$PKG_CHECKOUT" remote add origin https://github.com/ShowMeshSystems/fpp-showmesh.git
    git -C "$PKG_CHECKOUT" fetch -q origin "$FPP_SHOWMESH_COMMIT"
    git -C "$PKG_CHECKOUT" checkout -q FETCH_HEAD
fi
resolved_pkg_commit="$(git -C "$PKG_CHECKOUT" rev-parse HEAD)"
if [ "$resolved_pkg_commit" != "$FPP_SHOWMESH_COMMIT" ]; then
    echo "test-plugin-load-fpp-installer: local fpp-showmesh checkout resolved to '$resolved_pkg_commit', expected '$FPP_SHOWMESH_COMMIT'" >&2
    exit 1
fi
echo "test-plugin-load-fpp-installer: packaging repo checkout verified at commit $resolved_pkg_commit"

# ---------------------------------------------------------------------------
# Compiler-invocation probe: a wrapper placed ahead of the real compiler on
# PATH that appends one line to a marker log, then execs the real compiler.
# This makes "a compiler ran" positively observable (a new line appears)
# rather than something only ever inferred from a file's absence, in both
# directions: zero new lines proves NOT invoked, one or more proves invoked.
# ---------------------------------------------------------------------------

COMPILER_LOG=/tmp/showmesh-compiler-probe.log

install_compiler_probe() {
    docker exec "$CONTAINER" sh -c 'mkdir -p /usr/local/bin && rm -f '"$COMPILER_LOG"' && touch '"$COMPILER_LOG"
    for name in c++ g++ cc gcc; do
        real="$(docker exec "$CONTAINER" sh -c "command -v $name" 2>/dev/null || true)"
        # command -v inside the wrapper itself would find the wrapper first
        # once installed; resolve and hardcode the real path now, before
        # installing anything at /usr/local/bin.
        real="$(docker exec "$CONTAINER" sh -c "PATH=/usr/sbin:/usr/bin:/sbin:/bin command -v $name" 2>/dev/null || true)"
        if [ -z "$real" ]; then
            continue
        fi
        docker exec -i "$CONTAINER" sh -c "cat > /usr/local/bin/$name" <<WRAP
#!/bin/sh
echo "invoked: $name \$*" >> $COMPILER_LOG
exec "$real" "\$@"
WRAP
        docker exec "$CONTAINER" chmod 0755 "/usr/local/bin/$name"
    done
}

compiler_log_line_count() {
    docker exec "$CONTAINER" sh -c "wc -l < $COMPILER_LOG 2>/dev/null || echo 0" | tr -d '[:space:]'
}

compiler_log_since() {
    local offset="$1"
    docker exec "$CONTAINER" sh -c "tail -n +$((offset + 1)) $COMPILER_LOG 2>/dev/null || true"
}

install_compiler_probe

# ---------------------------------------------------------------------------
# Assertion plumbing shared with scripts/test-plugin-load-fpp.sh: the
# observable that a plugin is really dlopened by fppd is the registered
# command's presence in GET /api/commands, backed by the fppd.log load line.
# ---------------------------------------------------------------------------

COMMAND_NAME="ShowMesh: Set Brightness Ceiling"
API="http://localhost:${BENCH_HTTP_PORT}/api"
FPPD_LOG=/home/fpp/media/logs/fppd.log
PLUGIN_DIR="/home/fpp/media/plugins/fpp-showmesh"
PLUGIN_LOAD_OK_LINE="Processing Callbacks (${PLUGIN_DIR}/callbacks) for plugin: 'fpp-showmesh'"
PLUGIN_LOAD_FAIL_RE="Failed to load plugin|Failed to find shlib|Failed to load shlib|Failed to find  createPlugin|Failed to create plugin from shlib|Could not load plugin"

fppd_log_line_count() {
    docker exec "$CONTAINER" sh -c "wc -l < $FPPD_LOG 2>/dev/null || echo 0" | tr -d '[:space:]'
}

fppd_log_since() {
    local offset="$1"
    docker exec "$CONTAINER" sh -c "tail -n +$((offset + 1)) $FPPD_LOG 2>/dev/null || true"
}

command_json() {
    curl -fsS "${API}/commands" 2>/dev/null | jq -c --arg name "$COMMAND_NAME" '.[] | select(.name == $name)' 2>/dev/null || true
}

curl_retrying() {
    local tries=0 out=""
    while [ "$tries" -lt 5 ]; do
        if out="$(curl "$@")"; then
            printf '%s' "$out"
            return 0
        fi
        tries=$((tries + 1))
        sleep 1
    done
    printf '%s' "${out:-000}"
    return 0
}

invoke_command() {
    local target="$1" fade="$2"
    curl_retrying -sS -o "/tmp/showmesh-bench-invoke.$$" -w '%{http_code}' \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"${COMMAND_NAME}\",\"args\":[\"${target}\",\"${fade}\"],\"multisyncCommand\":false,\"multisyncHosts\":\"\"}" \
        "${API}/command"
}

invoke_command_body() {
    cat "/tmp/showmesh-bench-invoke.$$" 2>/dev/null || true
    rm -f "/tmp/showmesh-bench-invoke.$$" 2>/dev/null || true
}

assert_plugin_loaded() {
    local label="$1" offset json tail_log load_errs
    offset="$(fppd_log_line_count)"
    restart_and_wait
    json="$(command_json)"
    tail_log="$(fppd_log_since "$offset")"

    if [ -z "$json" ]; then
        echo "test-plugin-load-fpp-installer: FAIL $label -- command '$COMMAND_NAME' absent from GET /api/commands" >&2
        return 1
    fi
    if ! echo "$tail_log" | grep -qF "$PLUGIN_LOAD_OK_LINE"; then
        echo "test-plugin-load-fpp-installer: FAIL $label -- expected load line missing from this restart's fppd.log tail: $PLUGIN_LOAD_OK_LINE" >&2
        return 1
    fi
    load_errs="$(echo "$tail_log" | grep -E "$PLUGIN_LOAD_FAIL_RE" || true)"
    if [ -n "$load_errs" ]; then
        echo "test-plugin-load-fpp-installer: FAIL $label -- load-failure line present: $(echo "$load_errs" | head -3 | tr '\n' ' ')" >&2
        return 1
    fi

    local code body
    code="$(invoke_command 50 0)"; body="$(invoke_command_body)"
    if [ "$code" != "200" ] || ! echo "$body" | grep -qi "brightness ceiling set to 50 percent"; then
        echo "test-plugin-load-fpp-installer: FAIL $label -- brightness action did not answer: http=$code body=$body" >&2
        return 1
    fi

    echo "test-plugin-load-fpp-installer: PASS $label -- command registered, load line present, brightness action answered 200: $body"
    return 0
}

# ---------------------------------------------------------------------------
# Build a bench-local prebuilt fpp10 arm64 adapter object. Compiled in the
# SAME container, against its own /opt/fpp/src, via the identical
# `make -C native/adapters fpp10 ...` step the real installer's own compile
# path runs (native.sh's sm_native_compile) -- so this is the same build a
# host compiling the fpp10 adapter would produce, just done once up front and
# served back rather than rebuilt for every install. Not built through
# scripts/build-prebuilt-fpp10.sh's docker buildx pipeline, which pulls a
# fresh debian:trixie base image; this Mac's Docker Desktop VM has too little
# free disk for that pull.
# ---------------------------------------------------------------------------

PREBUILT_WORKDIR="/tmp/showmesh-prebuilt-build"
PREBUILT_SONAME="libshowmesh-fpp10-arm64.so"

build_prebuilt_fpp10_object() {
    echo "test-plugin-load-fpp-installer: building the bench-local prebuilt fpp10 arm64 object (not counted against the compiler-invocation assertion; built before the probe log offset is taken for any case)"
    docker exec "$CONTAINER" rm -rf "$PREBUILT_WORKDIR"
    docker exec "$CONTAINER" cp -r /opt/showmesh-native-src "$PREBUILT_WORKDIR"
    docker exec "$CONTAINER" make -C "$PREBUILT_WORKDIR/adapters" fpp10 \
        FPP_SRC=/opt/fpp/src \
        CXXFLAGS="-std=c++20 -O2 -fPIC -Wall -Wextra -Werror -fno-gnu-unique"
    rm -rf "/tmp/showmesh-prebuilt-obj-${BENCH_ID}"
    mkdir -p "/tmp/showmesh-prebuilt-obj-${BENCH_ID}"
    docker cp "${CONTAINER}:${PREBUILT_WORKDIR}/adapters/build/fpp10/libshowmesh-fpp10.so" "/tmp/showmesh-prebuilt-obj-${BENCH_ID}/${PREBUILT_SONAME}"
    docker exec "$CONTAINER" rm -rf "$PREBUILT_WORKDIR"
    sha256_of "/tmp/showmesh-prebuilt-obj-${BENCH_ID}/${PREBUILT_SONAME}"
}

# ---------------------------------------------------------------------------
# Serves the prebuilt object over a LOCAL http server started INSIDE the
# bench container on loopback, per the standing bench ruling: nothing is
# published, and nothing is fetched from a published location. Named
# process trick mirrors scripts/test-plugin-load-fpp.sh's DDP capture
# listener, so it can be stopped by name without matching an unrelated
# python3 process.
# ---------------------------------------------------------------------------

SERVE_PROC_NAME="showmesh-artifact-serve"
SERVE_BIN="/tmp/${SERVE_PROC_NAME}"
SERVE_PORT=8899
SERVE_DIR=/tmp/showmesh-artifact-serve-root

stop_local_server() {
    docker exec "$CONTAINER" pkill -x "$SERVE_PROC_NAME" >/dev/null 2>&1 || true
    local i=0
    while [ "$i" -lt 20 ]; do
        docker exec "$CONTAINER" pgrep -x "$SERVE_PROC_NAME" >/dev/null 2>&1 || return 0
        i=$((i + 1))
        sleep 0.5
    done
    return 1
}

start_local_server() {
    stop_local_server || true
    docker exec "$CONTAINER" mkdir -p "$SERVE_DIR"
    docker exec "$CONTAINER" sh -c "ln -sf \"\$(PATH=/usr/sbin:/usr/bin:/sbin:/bin command -v python3)\" '$SERVE_BIN'"
    docker exec -d -w "$SERVE_DIR" "$CONTAINER" "$SERVE_BIN" -m http.server "$SERVE_PORT" --bind 127.0.0.1
    local i=0
    while [ "$i" -lt 30 ]; do
        if docker exec "$CONTAINER" sh -c "curl -fsS -o /dev/null http://127.0.0.1:${SERVE_PORT}/ 2>/dev/null"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.5
    done
    return 1
}

# ---------------------------------------------------------------------------
# Assemble the plugin directory the real installer will run against: an
# unmodified copy of the pinned fpp-showmesh checkout, plus this bench's own
# override files (lock + base URL, both mechanisms the installer already
# supports for exactly this purpose) and, only for the fpp10 case, a
# bench-chosen fpp10-verified-versions.txt content.
# ---------------------------------------------------------------------------

BENCH_VERSION="0.1.3-bench"
STATE_DIR="/home/fpp/media/plugindata/fpp-showmesh"

write_go_helper_fixture() {
    local workdir="$1"
    mkdir -p "$workdir"
    printf '#!/bin/sh\nexit 0\n' > "$workdir/showmesh-fpp-plugin"
    chmod 0755 "$workdir/showmesh-fpp-plugin"
    tar -C "$workdir" -czf "$workdir/showmesh-fpp-plugin_${BENCH_VERSION}_linux_arm64.tar.gz" showmesh-fpp-plugin
    sha256_of "$workdir/showmesh-fpp-plugin_${BENCH_VERSION}_linux_arm64.tar.gz"
}

# The compile fallback (sm_install_native's non-prebuilt path) fetches a
# native-source tarball and compiles it, rather than compiling something
# already on disk; it needs a real, buildable native/ tree, not a stub. This
# packages the actual native/ directory this repository ships (the same
# tree scripts/test-plugin-load-fpp.sh's own hand-rolled install compiles),
# unmodified, so the compile the real installer runs is a real compile of
# this repository's real adapter sources.
write_native_source_fixture() {
    local workdir="$1"
    mkdir -p "$workdir"
    tar -C "$REPO_ROOT" -czf "$workdir/showmesh-fpp-plugin-native_${BENCH_VERSION}.tar.gz" native
    sha256_of "$workdir/showmesh-fpp-plugin-native_${BENCH_VERSION}.tar.gz"
}

install_plugin_tree() {
    local not_verified="$1"

    docker exec "$CONTAINER" rm -rf "$PLUGIN_DIR"
    docker exec "$CONTAINER" mkdir -p "$PLUGIN_DIR"
    local entry
    for entry in "$PKG_CHECKOUT"/*; do
        [ -e "$entry" ] || continue
        docker cp "$entry" "${CONTAINER}:${PLUGIN_DIR}/"
    done
    docker exec "$CONTAINER" chmod 0755 \
        "${PLUGIN_DIR}/scripts/fpp_install.sh" \
        "${PLUGIN_DIR}/scripts/fpp_upgrade.sh" \
        "${PLUGIN_DIR}/scripts/fpp_uninstall.sh" \
        "${PLUGIN_DIR}/scripts/preStart.sh" \
        "${PLUGIN_DIR}/callbacks"

    printf '%s\n' "$BENCH_VERSION" | docker exec -i "$CONTAINER" sh -c "cat > ${PLUGIN_DIR}/VERSION"

    if [ "$BENCH_FPP_MAJOR" = "fpp10" ]; then
        if [ "$not_verified" = "1" ]; then
            docker exec -i "$CONTAINER" sh -c "cat > ${PLUGIN_DIR}/fpp10-verified-versions.txt" <<'EOF'
# Bench-owned fixture (scripts/test-plugin-load-fpp-installer.sh, --verified
# no): deliberately does not list this container's FPP version, so
# sm_fpp10_version_verified fails and sm_install_native falls through to
# compiling. This is not the committed fpp-showmesh file.
10.255.255
EOF
        else
            docker exec -i "$CONTAINER" sh -c "cat > ${PLUGIN_DIR}/fpp10-verified-versions.txt" <<'EOF'
# Bench-owned fixture (scripts/test-plugin-load-fpp-installer.sh, --verified
# yes): lists exactly the FPP version this container reports
# (getFPPVersionTriplet() at the pinned FPP 10.0 tag), matching real
# fpp-showmesh's own fpp10-verified-versions.txt entry for 10.0.0. This is
# a copy for bench clarity, not the committed file.
10.0.0
EOF
        fi
    fi

    docker exec "$CONTAINER" mkdir -p "$STATE_DIR"

    # Raise LogLevel_Plugin before the restart that loads the plugin, same as
    # scripts/test-plugin-load-fpp.sh's own install step: PLUGIN_LOAD_OK_LINE
    # is only emitted at debug level, and a load failure must also be
    # visible. Bench-owned accommodation, not a product setting.
    docker exec "$CONTAINER" sh -c \
        "grep -q '^LogLevel_Plugin' /home/fpp/media/settings 2>/dev/null && \
         sed -i 's/^LogLevel_Plugin.*/LogLevel_Plugin = debug/' /home/fpp/media/settings || \
         echo 'LogLevel_Plugin = debug' >> /home/fpp/media/settings"
}

# $1 = 0/1 whether the lock+base-url override should point at a real,
# digest-matching prebuilt object (only meaningful for fpp10).
configure_prebuilt_source() {
    local object_sha="$1"
    local workdir="/tmp/showmesh-lock-${BENCH_ID}"
    rm -rf "$workdir"
    mkdir -p "$workdir"
    local go_sha native_sha
    go_sha="$(write_go_helper_fixture "$workdir")"
    native_sha="$(write_native_source_fixture "$workdir")"

    cat > "$workdir/artifacts.lock.json" <<JSON
{
  "note": "Bench-owned override lock (scripts/test-plugin-load-fpp-installer.sh). The go-helper artifact is a no-op fixture: this bench proves the native/resident component only. Never published, never fetched from a published location.",
  "version": "${BENCH_VERSION}",
  "artifacts": [
    { "filename": "showmesh-fpp-plugin_${BENCH_VERSION}_linux_arm64.tar.gz", "kind": "go-helper", "architecture": "arm64", "sha256": "${go_sha}" },
    { "filename": "showmesh-fpp-plugin-native_${BENCH_VERSION}.tar.gz", "kind": "native-source", "architecture": "any", "sha256": "${native_sha}" },
    { "filename": "${PREBUILT_SONAME}", "kind": "native-prebuilt-fpp10", "architecture": "arm64", "sha256": "${object_sha}" }
  ]
}
JSON

    docker cp "$workdir/showmesh-fpp-plugin_${BENCH_VERSION}_linux_arm64.tar.gz" "${CONTAINER}:${SERVE_DIR}/"
    docker cp "$workdir/showmesh-fpp-plugin-native_${BENCH_VERSION}.tar.gz" "${CONTAINER}:${SERVE_DIR}/"
    docker cp "/tmp/showmesh-prebuilt-obj-${BENCH_ID}/${PREBUILT_SONAME}" "${CONTAINER}:${SERVE_DIR}/"
    docker cp "$workdir/artifacts.lock.json" "${CONTAINER}:${STATE_DIR}/artifact-lock-path.lock.json"
    printf '%s' "${STATE_DIR}/artifact-lock-path.lock.json" | docker exec -i "$CONTAINER" sh -c "cat > ${STATE_DIR}/artifact-lock-path"
    printf 'http://127.0.0.1:%s' "$SERVE_PORT" | docker exec -i "$CONTAINER" sh -c "cat > ${STATE_DIR}/artifact-base-url"
    rm -rf "$workdir"
}

# fpp9, and the fpp10 compile-fallback case, still need the go-helper
# fixture and its lock entry reachable, but never need the prebuilt object
# or an artifact base URL override: the default
# https://github.com/ShowMeshSystems/showmesh/... base URL, with SHOWMESH_
# PLUGIN_ARTIFACT_BASE_URL left unset, would reach the network, so a local
# override is still required, it just never needs to carry a prebuilt entry.
configure_compile_only_source() {
    local workdir="/tmp/showmesh-lock-${BENCH_ID}"
    rm -rf "$workdir"
    mkdir -p "$workdir"
    local go_sha native_sha
    go_sha="$(write_go_helper_fixture "$workdir")"
    native_sha="$(write_native_source_fixture "$workdir")"

    cat > "$workdir/artifacts.lock.json" <<JSON
{
  "note": "Bench-owned override lock (scripts/test-plugin-load-fpp-installer.sh). The go-helper artifact is a no-op fixture: this bench proves the native/resident component only. Never published, never fetched from a published location.",
  "version": "${BENCH_VERSION}",
  "artifacts": [
    { "filename": "showmesh-fpp-plugin_${BENCH_VERSION}_linux_arm64.tar.gz", "kind": "go-helper", "architecture": "arm64", "sha256": "${go_sha}" },
    { "filename": "showmesh-fpp-plugin-native_${BENCH_VERSION}.tar.gz", "kind": "native-source", "architecture": "any", "sha256": "${native_sha}" }
  ]
}
JSON

    docker cp "$workdir/showmesh-fpp-plugin_${BENCH_VERSION}_linux_arm64.tar.gz" "${CONTAINER}:${SERVE_DIR}/"
    docker cp "$workdir/showmesh-fpp-plugin-native_${BENCH_VERSION}.tar.gz" "${CONTAINER}:${SERVE_DIR}/"
    docker cp "$workdir/artifacts.lock.json" "${CONTAINER}:${STATE_DIR}/artifact-lock-path.lock.json"
    printf '%s' "${STATE_DIR}/artifact-lock-path.lock.json" | docker exec -i "$CONTAINER" sh -c "cat > ${STATE_DIR}/artifact-lock-path"
    printf 'http://127.0.0.1:%s' "$SERVE_PORT" | docker exec -i "$CONTAINER" sh -c "cat > ${STATE_DIR}/artifact-base-url"
    rm -rf "$workdir"
}

run_real_installer() {
    echo "test-plugin-load-fpp-installer: running fpp-showmesh's own scripts/fpp_install.sh inside the container (SM_REQUIRE_NATIVE=1)"
    docker exec -e SM_REQUIRE_NATIVE=1 "$CONTAINER" sh "${PLUGIN_DIR}/scripts/fpp_install.sh" "FPPDIR=/opt/fpp"
}

# ---------------------------------------------------------------------------
# Drive one case end to end.
# ---------------------------------------------------------------------------

RESULT_ASSERT=""
RESULT_COMPILER=""

run_case() {
    local label="$1" not_verified="$2" expect_compiler="$3"
    if [ "$FORCE_EXPECT_NO_COMPILER" = "1" ]; then
        expect_compiler="no"
        label="${label}_DEMO_forced_expect_no_compiler"
    fi

    start_local_server
    install_plugin_tree "$not_verified"

    if [ "$BENCH_FPP_MAJOR" = "fpp10" ] && [ "$not_verified" != "always-compile-only" ]; then
        local prebuilt_sha
        prebuilt_sha="$(build_prebuilt_fpp10_object | tail -1)"
        echo "test-plugin-load-fpp-installer: bench-local prebuilt object sha256 $prebuilt_sha"
        configure_prebuilt_source "$prebuilt_sha"
    else
        configure_compile_only_source
    fi

    local probe_offset
    probe_offset="$(compiler_log_line_count)"
    run_real_installer
    local probe_tail invoked
    probe_tail="$(compiler_log_since "$probe_offset")"
    if [ -n "$probe_tail" ]; then
        invoked="yes"
    else
        invoked="no"
    fi
    echo "test-plugin-load-fpp-installer: compiler-invocation probe for $label: invoked=$invoked"
    if [ -n "$probe_tail" ]; then
        echo "test-plugin-load-fpp-installer: probe log tail:"
        echo "$probe_tail" | sed 's/^/    /'
    fi

    if [ "$invoked" = "$expect_compiler" ]; then
        RESULT_COMPILER="PASS"
        echo "test-plugin-load-fpp-installer: PASS ${label}_no_compiler_assertion -- expected invoked=$expect_compiler, observed invoked=$invoked"
    else
        RESULT_COMPILER="FAIL"
        echo "test-plugin-load-fpp-installer: FAIL ${label}_no_compiler_assertion -- expected invoked=$expect_compiler, observed invoked=$invoked" >&2
    fi

    if assert_plugin_loaded "$label"; then
        RESULT_ASSERT="PASS"
    else
        RESULT_ASSERT="FAIL"
    fi

    stop_local_server || true
}

echo ""
echo "test-plugin-load-fpp-installer: === case: major=$BENCH_FPP_MAJOR verified=$BENCH_VERIFIED ==="
if [ "$BENCH_FPP_MAJOR" = "fpp10" ]; then
    if [ "$BENCH_VERIFIED" = "yes" ]; then
        run_case "prebuilt_${BENCH_ID}" "0" "no"
    else
        run_case "fallback_${BENCH_ID}" "1" "yes"
    fi
else
    run_case "fpp9_${BENCH_ID}" "always-compile-only" "yes"
fi

echo ""
echo "test-plugin-load-fpp-installer: summary for major=$BENCH_FPP_MAJOR id=$BENCH_ID verified=$BENCH_VERIFIED"
echo "  $RESULT_ASSERT  plugin_loaded_and_brightness_answers"
echo "  $RESULT_COMPILER  no_compiler_assertion"

if [ "$RESULT_ASSERT" != "PASS" ] || [ "$RESULT_COMPILER" != "PASS" ]; then
    exit 1
fi
exit 0
