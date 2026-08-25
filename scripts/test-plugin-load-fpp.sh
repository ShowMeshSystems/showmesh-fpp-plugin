#!/usr/bin/env bash
# Bench scaffolding, not the product. Stands up a containerized fppd (FPP 9
# or FPP 10), performs a bench-owned install of the ShowMesh plugin adapter
# into it, and runs one named assertion per behavior, reporting PASS or FAIL
# for each and exiting nonzero if any failed.
#
# Never modifies anything under native/: the adapter is compiled in-container
# against that container's own /opt/fpp/src, and any extra build flag is
# passed in from here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCH_DIR="$REPO_ROOT/bench/fpp-plugin-load"

# ---------------------------------------------------------------------------
# Argument / environment parsing
# ---------------------------------------------------------------------------

BENCH_ID="${BENCH_ID:-local}"
BENCH_HTTP_PORT="${BENCH_HTTP_PORT:-8190}"
BENCH_FPP_MAJOR="${BENCH_FPP_MAJOR:-fpp9}"
BENCH_CPU="${BENCH_CPU:-amd64}"
BENCH_USE_PREBUILT="${BENCH_USE_PREBUILT:-0}"
DOWN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  --id ID        Isolates this run (container, network, volume, project
                 name). Default: \$BENCH_ID or "local".
  --port PORT    Host port for the container's HTTP API. Default:
                 \$BENCH_HTTP_PORT or 8190.
  --major MAJOR  fpp9 or fpp10. Default: \$BENCH_FPP_MAJOR or fpp9.
  --cpu CPU      amd64 or arm64. Default: \$BENCH_CPU or amd64, so existing
                 recorded results keep their meaning. arm64 is a native run
                 on an arm64 host; the image tag is suffixed "-arm64" so it
                 can never collide with an amd64 build of the same FPP tag.
                 Refuses to run if a cached image under the resolved tag is
                 not actually built for the requested CPU.
  --prebuilt     Use the private prebuilt FPP 9 fixture image instead of
                 building from source. Requires FPP_PREBUILT_IMAGE; see
                 bench/fpp-plugin-load/.env.example. Refused for fpp10:
                 there is no fixture image for it. Refused for --cpu arm64:
                 the fixture is amd64 only.
  --down         Tear this run down and exit. Removes the container, the
                 network AND the named media volume (docker compose down -v),
                 so the next run with this --id starts from a clean fppd
                 state. Leaves other runs (different --id) untouched.
  -h, --help     This message.

Leaves the container running at the end of a normal run; the image build
is expensive and this mirrors the sibling repo's own bench. Run with
--down when you are done with this --id.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --id) BENCH_ID="$2"; shift 2 ;;
        --port) BENCH_HTTP_PORT="$2"; shift 2 ;;
        --major) BENCH_FPP_MAJOR="$2"; shift 2 ;;
        --cpu) BENCH_CPU="$2"; shift 2 ;;
        --prebuilt) BENCH_USE_PREBUILT=1; shift ;;
        --down) DOWN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for dep in docker curl jq; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        echo "test-plugin-load-fpp: required dependency '$dep' is not on PATH" >&2
        exit 1
    fi
done

case "$BENCH_FPP_MAJOR" in
    fpp9)
        FPP_TAG="9.5.3"
        FPP_COMMIT="7979a4bb0bb9068fea71f3b447e273d5c0ea01e3"
        FPP_IMAGE="showmesh-bench/fpp:9.5.3"
        ADAPTER_TARGET="fpp9"
        SO_NAME="libshowmesh-fpp9.so"
        EXTRA_CXXFLAGS=""
        ;;
    fpp10)
        FPP_TAG="10.0"
        FPP_COMMIT="370e62ed7e8c8318da6ee5b01312b8b75082d952"
        FPP_IMAGE="showmesh-bench/fpp:10.0"
        ADAPTER_TARGET="fpp10"
        SO_NAME="libshowmesh-fpp10.so"
        # dlclose() unmaps nothing for a plugin declaring
        # FPP_PLUGIN_SUPPORTS_UNLOAD unless it is built with this flag.
        # Passed from the bench so native/adapters/Makefile stays untouched.
        EXTRA_CXXFLAGS="-fno-gnu-unique"
        ;;
    *)
        echo "test-plugin-load-fpp: --major must be fpp9 or fpp10, got '$BENCH_FPP_MAJOR'" >&2
        exit 2
        ;;
esac

case "$BENCH_CPU" in
    amd64)
        FPP_PLATFORM="linux/amd64"
        ;;
    arm64)
        FPP_PLATFORM="linux/arm64"
        # Keyed by architecture so an amd64 and an arm64 build of the same
        # FPP tag can never collide under one image name, the way
        # showmesh-bench/fpp:9.5.3-arm64 already did by hand before this.
        FPP_IMAGE="${FPP_IMAGE}-arm64"
        ;;
    *)
        echo "test-plugin-load-fpp: --cpu must be amd64 or arm64, got '$BENCH_CPU'" >&2
        exit 2
        ;;
esac

if [ "$BENCH_USE_PREBUILT" = "1" ] && [ "$BENCH_FPP_MAJOR" = "fpp10" ]; then
    echo "test-plugin-load-fpp: --prebuilt has no FPP 10 fixture image; FPP 10 is source-build only" >&2
    exit 2
fi

if [ "$BENCH_USE_PREBUILT" = "1" ] && [ "$BENCH_CPU" = "arm64" ]; then
    echo "test-plugin-load-fpp: --prebuilt has no arm64 fixture image; the fixture is amd64 only" >&2
    exit 2
fi

# Normalizes both docker's image-architecture vocabulary (amd64/arm64) and
# uname -m's (x86_64/amd64, arm64/aarch64) to the same two strings, so the
# host and image architectures can be compared directly below.
normalize_arch() {
    case "$1" in
        x86_64|amd64) echo "amd64" ;;
        arm64|aarch64) echo "arm64" ;;
        *) echo "$1" ;;
    esac
}

# The build context must be a LOCAL checkout carrying a real .git directory
# and a ref literally named by the pinned tag: EXTRA_INSTALL_FLAG=--skip-clone
# is required to keep the pinned tree, and both src/fppversion.sh and
# SD/FPP_Install.sh's --skip-clone path shell out to git. A remote git-URL
# context supplies neither, and both failures were observed directly: a
# working tree exported with no .git, and "pathspec '10.0-beta5' did not
# match any file(s) known to git" for a checkout fetched by bare commit SHA.
#
# Keyed by BENCH_ID as well as major: two concurrent runs of the same major
# would otherwise both rm -rf/fetch the one shared ${BENCH_FPP_MAJOR}
# checkout dir and race. The cost is that every run re-fetches its own
# shallow, single-tag copy instead of reusing one shared per-major checkout.
if [ "$BENCH_USE_PREBUILT" != "1" ] && docker image inspect "$FPP_IMAGE" >/dev/null 2>&1; then
    # An already-present tag is reused as-is rather than rebuilt, so it must
    # actually be the requested architecture. Without this, a tag built once
    # under the host default (or under a stale --cpu) would run silently
    # under the wrong CPU type forever after.
    cached_arch="$(docker image inspect "$FPP_IMAGE" --format '{{.Architecture}}')"
    if [ "$cached_arch" != "$BENCH_CPU" ]; then
        echo "test-plugin-load-fpp: REFUSING to run: $FPP_IMAGE is already cached as architecture '$cached_arch', but --cpu $BENCH_CPU was requested. Remove or re-tag the mismatched image (or pass the matching --cpu) rather than silently running the wrong CPU type." >&2
        exit 1
    fi
    # No build will happen, but compose still needs a value to interpolate.
    FPP_BUILD_CONTEXT="$BENCH_DIR/.fpp-src/${BENCH_FPP_MAJOR}-${BENCH_ID}"
elif [ "$BENCH_USE_PREBUILT" != "1" ]; then
    checkout_dir="$BENCH_DIR/.fpp-src/${BENCH_FPP_MAJOR}-${BENCH_ID}"
    if [ ! -d "$checkout_dir/.git" ] || [ "$(git -C "$checkout_dir" rev-parse HEAD 2>/dev/null)" != "$FPP_COMMIT" ]; then
        echo "test-plugin-load-fpp: preparing a local, commit-pinned $BENCH_FPP_MAJOR checkout at $checkout_dir"
        rm -rf "$checkout_dir"
        mkdir -p "$checkout_dir"
        git -C "$checkout_dir" init -q
        git -C "$checkout_dir" remote add origin https://github.com/FalconChristmas/fpp.git
        if ! git -C "$checkout_dir" fetch --depth 1 origin "refs/tags/${FPP_TAG}:refs/tags/${FPP_TAG}"; then
            echo "test-plugin-load-fpp: FAILED to fetch tag $FPP_TAG from FalconChristmas/fpp" >&2
            exit 1
        fi
        git -C "$checkout_dir" checkout -q "$FPP_TAG"
    fi
    # A tag is a moving name; the pin is the commit, so verify the commit.
    resolved_commit="$(git -C "$checkout_dir" rev-parse HEAD 2>/dev/null || echo '')"
    if [ "$resolved_commit" != "$FPP_COMMIT" ]; then
        echo "test-plugin-load-fpp: local $BENCH_FPP_MAJOR checkout's tag $FPP_TAG resolved to '$resolved_commit', expected '$FPP_COMMIT'" >&2
        exit 1
    fi
    echo "test-plugin-load-fpp: local $BENCH_FPP_MAJOR checkout verified: tag $FPP_TAG resolves to commit $resolved_commit"
    FPP_BUILD_CONTEXT="$checkout_dir"
else
    # No build will happen, but compose still needs a value to interpolate.
    FPP_BUILD_CONTEXT="$BENCH_DIR/.fpp-src/${BENCH_FPP_MAJOR}-${BENCH_ID}"
fi

export FPP_IMAGE FPP_TAG FPP_COMMIT FPP_BUILD_CONTEXT FPP_PLATFORM BENCH_ID BENCH_HTTP_PORT

PROJECT="showmesh-fppbench-${BENCH_ID}"
CONTAINER="showmesh-fppbench-${BENCH_ID}-fpp"
COMPOSE=(docker compose -p "$PROJECT" -f "$BENCH_DIR/docker-compose.yml")
if [ "$BENCH_USE_PREBUILT" = "1" ]; then
    COMPOSE+=(-f "$BENCH_DIR/docker-compose.prebuilt.yml")
fi

if [ "$DOWN" = "1" ]; then
    # -v removes the named media volume too. Without it fppd.log, the
    # settings file and the installed plugin survive into the next run with
    # the same --id, so --down would not be a reset.
    echo "test-plugin-load-fpp: tearing down $PROJECT (including its named media volume)"
    "${COMPOSE[@]}" down -v
    exit 0
fi

# ---------------------------------------------------------------------------
# Result accounting, summary, and the EXIT trap
# ---------------------------------------------------------------------------

RESULTS_NAMES=()
RESULTS_STATUS=()
EXPECTED_ASSERTIONS=8
SUMMARY_PRINTED=0
SUMMARY_FAILED=0

record() {
    local name="$1" status="$2" detail="${3:-}"
    RESULTS_NAMES+=("$name")
    RESULTS_STATUS+=("$status")
    if [ "$status" = "PASS" ]; then
        # Print the detail on PASS too: what was actually measured is the
        # evidence, and it is worthless if only failures ever show it.
        if [ -n "$detail" ]; then
            echo "test-plugin-load-fpp: PASS $name -- $detail"
        else
            echo "test-plugin-load-fpp: PASS $name"
        fi
    else
        echo "test-plugin-load-fpp: FAIL $name -- $detail"
    fi
}

print_summary() {
    if [ "$SUMMARY_PRINTED" = "1" ]; then
        return 0
    fi
    SUMMARY_PRINTED=1
    local total=${#RESULTS_NAMES[@]}
    echo ""
    echo "test-plugin-load-fpp: summary for major=$BENCH_FPP_MAJOR id=$BENCH_ID"
    if [ "$total" -eq 0 ]; then
        echo "test-plugin-load-fpp: ran ZERO assertions; treating this as a hard failure rather than a quiet, misleadingly-green exit" >&2
        SUMMARY_FAILED=1
        return 0
    fi
    local i
    for i in "${!RESULTS_NAMES[@]}"; do
        echo "  ${RESULTS_STATUS[$i]}  ${RESULTS_NAMES[$i]}"
        if [ "${RESULTS_STATUS[$i]}" != "PASS" ]; then
            SUMMARY_FAILED=$((SUMMARY_FAILED + 1))
        fi
    done
    if [ "$total" -lt "$EXPECTED_ASSERTIONS" ]; then
        echo "test-plugin-load-fpp: only $total of $EXPECTED_ASSERTIONS assertions recorded a result; the run ended early and the rest were NOT evaluated" >&2
        SUMMARY_FAILED=$((SUMMARY_FAILED + 1))
    fi
    if [ "$SUMMARY_FAILED" -ne 0 ]; then
        echo "test-plugin-load-fpp: $SUMMARY_FAILED of $total assertions FAILED"
    else
        echo "test-plugin-load-fpp: all $total assertions PASSED"
    fi
}

# Every abort path (a `set -e` death, an unexpected docker exec failure, a
# restart that never came back) must still leave the accumulated results
# visible and the host temp files cleaned up.
on_exit() {
    local rc=$?
    print_summary || true
    rm -f "/tmp/showmesh-bench-invoke.$$" "/tmp/showmesh-bench-unload.$$" 2>/dev/null || true
    exit "$rc"
}
trap on_exit EXIT

# ---------------------------------------------------------------------------
# Bring the container up
# ---------------------------------------------------------------------------

if [ "$BENCH_USE_PREBUILT" = "1" ]; then
    echo "test-plugin-load-fpp: --prebuilt selected; pulling before up so a failed pull never silently falls back to a source build"
    if ! "${COMPOSE[@]}" pull fpp; then
        echo "test-plugin-load-fpp: FAILED to pull the prebuilt fixture image; refusing to fall back to a source build" >&2
        exit 1
    fi
fi

echo "test-plugin-load-fpp: FPP major=$BENCH_FPP_MAJOR tag=$FPP_TAG commit=$FPP_COMMIT image=$FPP_IMAGE platform=$FPP_PLATFORM"

# --force-recreate discards the previous container's writable layer, where the
# stale apache/php pid file that crash-loops a plain second `up -d` lives, while
# keeping the built image and the named media volume.
echo "test-plugin-load-fpp: docker compose up -d --force-recreate"
"${COMPOSE[@]}" up -d --force-recreate

# The image is guaranteed to exist now, built or pulled by the up above, so
# its real architecture and reference are read from docker itself rather than
# assumed from --cpu or the host default.
resolved_ref="$(docker image inspect "$FPP_IMAGE" --format '{{index .RepoDigests 0}}' 2>/dev/null || true)"
if [ -z "$resolved_ref" ]; then
    resolved_ref="$(docker image inspect "$FPP_IMAGE" --format '{{.Id}}' 2>/dev/null || echo 'unknown')"
    echo "test-plugin-load-fpp: resolved image reference: $resolved_ref (LOCAL BUILD OUTPUT, not a published digest anyone else can pull; do not present this as a reproducible pin)"
else
    echo "test-plugin-load-fpp: resolved image reference: $resolved_ref"
fi

IMAGE_ARCH="$(docker image inspect "$FPP_IMAGE" --format '{{.Architecture}}' 2>/dev/null || echo 'unknown')"
if [ "$IMAGE_ARCH" != "$BENCH_CPU" ]; then
    # A build honoring the pinned platform: key above should never produce
    # this, but it is cheap to confirm rather than trust, and it is exactly
    # the class of silent mismatch this whole change exists to close.
    echo "test-plugin-load-fpp: FATAL: $FPP_IMAGE was built/pulled as architecture '$IMAGE_ARCH', not the requested '$BENCH_CPU'" >&2
    exit 1
fi

HOST_ARCH="$(uname -m)"
HOST_ARCH_NORM="$(normalize_arch "$HOST_ARCH")"
echo "test-plugin-load-fpp: host architecture: $HOST_ARCH, container image architecture: $IMAGE_ARCH (platform $FPP_PLATFORM)"
if [ "$IMAGE_ARCH" = "$HOST_ARCH_NORM" ]; then
    EMULATED=0
    echo "test-plugin-load-fpp: NATIVE RUN, the container image architecture matches the host architecture, so this container runs with no CPU emulation layer."
else
    EMULATED=1
    echo "test-plugin-load-fpp: EMULATED RUN, host arch '$HOST_ARCH' does not match the container image architecture '$IMAGE_ARCH', so this container runs under emulation. Any duration this run reports is inflated by an unmeasured factor and is not evidence of real-host or real-fleet-hardware timing. Nothing here speaks to native arm64 or armv7 behavior on real fleet hardware."
fi

wait_for_http() {
    local tries="${1:-60}"
    local i=0
    while [ "$i" -lt "$tries" ]; do
        if curl -fsS -o /dev/null --max-time 2 "http://localhost:${BENCH_HTTP_PORT}/api/fppd/status"; then
            return 0
        fi
        i=$((i + 1))
        sleep 2
    done
    return 1
}

echo "test-plugin-load-fpp: waiting for http://localhost:${BENCH_HTTP_PORT}/api/fppd/status"
if ! wait_for_http 90; then
    echo "test-plugin-load-fpp: fppd did not answer within the wait budget" >&2
    docker logs "$CONTAINER" --tail 100 >&2 || true
    exit 1
fi

# ---------------------------------------------------------------------------
# Bench-owned install
# ---------------------------------------------------------------------------

PLUGIN_DIR="/home/fpp/media/plugins/fpp-showmesh"

install_plugin() {
    echo "test-plugin-load-fpp: installing (bench-owned, not the real installer)"
    docker exec "$CONTAINER" rm -rf /tmp/showmesh-native
    docker exec "$CONTAINER" cp -r /opt/showmesh-native-src /tmp/showmesh-native
    docker exec "$CONTAINER" mkdir -p "$PLUGIN_DIR"

    local start_ns end_ns duration_ms
    start_ns=$(date +%s%N)
    docker exec "$CONTAINER" make -C /tmp/showmesh-native/adapters "$ADAPTER_TARGET" \
        FPP_SRC=/opt/fpp/src \
        CXXFLAGS="-std=c++20 -O2 -fPIC -Wall -Wextra -Werror ${EXTRA_CXXFLAGS}"
    end_ns=$(date +%s%N)
    duration_ms=$(( (end_ns - start_ns) / 1000000 ))
    if [ "${EMULATED:-0}" = "1" ]; then
        echo "test-plugin-load-fpp: in-container adapter compile took ${duration_ms}ms (measured under emulation, container image architecture '$IMAGE_ARCH' on host arch '$HOST_ARCH', NOT representative of a real host and not a basis for a packaging time estimate)"
    else
        echo "test-plugin-load-fpp: in-container adapter compile took ${duration_ms}ms"
    fi

    docker exec "$CONTAINER" cp "/tmp/showmesh-native/adapters/build/${ADAPTER_TARGET}/${SO_NAME}" "${PLUGIN_DIR}/${SO_NAME}"
    docker exec "$CONTAINER" cp /opt/showmesh-plugin-src/callbacks "${PLUGIN_DIR}/callbacks"
    docker exec "$CONTAINER" chmod 0755 "${PLUGIN_DIR}/callbacks"

    # Raise LogLevel_Plugin before the restart that loads the plugin so a load
    # failure is visible. Idempotent: replaces rather than appends.
    docker exec "$CONTAINER" sh -c \
        "grep -q '^LogLevel_Plugin' /home/fpp/media/settings 2>/dev/null && \
         sed -i 's/^LogLevel_Plugin.*/LogLevel_Plugin = debug/' /home/fpp/media/settings || \
         echo 'LogLevel_Plugin = debug' >> /home/fpp/media/settings"

    # UDPOutput::Init() disables any output whose destination it recognizes as
    # one of the container's own addresses, 127.0.0.1 included. This bench
    # deliberately captures on loopback inside the sending container, so it
    # needs that check skipped. Bench-owned accommodation, not a product setting.
    docker exec "$CONTAINER" sh -c \
        "grep -q '^DisableFakeNetworkBridges' /home/fpp/media/settings 2>/dev/null && \
         sed -i 's/^DisableFakeNetworkBridges.*/DisableFakeNetworkBridges = 1/' /home/fpp/media/settings || \
         echo 'DisableFakeNetworkBridges = 1' >> /home/fpp/media/settings"
}

restart_and_wait() {
    # `docker restart` crash-loops this image on a stale apache/php pid file;
    # every restart in this script goes through --force-recreate instead.
    echo "test-plugin-load-fpp: recreating container (fppd re-scans /home/fpp/media/plugins on start)"
    "${COMPOSE[@]}" up -d --force-recreate >/dev/null
    if ! wait_for_http 90; then
        echo "test-plugin-load-fpp: fppd did not come back up after restart" >&2
        docker logs "$CONTAINER" --tail 100 >&2 || true
        exit 1
    fi
}

install_plugin

# ---------------------------------------------------------------------------
# Assertion helpers
# ---------------------------------------------------------------------------

COMMAND_NAME="ShowMesh: Set Brightness Ceiling"
API="http://localhost:${BENCH_HTTP_PORT}/api"
FPPD_LOG=/home/fpp/media/logs/fppd.log

# fppd.log is appended to and never truncated across restarts and across runs
# sharing a --id's media volume, so every log assertion works on the tail
# written after a recorded offset, never on the whole file or a fixed tail -N.
fppd_log_line_count() {
    local n
    n="$(docker exec "$CONTAINER" sh -c "wc -l < $FPPD_LOG 2>/dev/null || echo 0" | tr -d '[:space:]')"
    echo "${n:-0}"
}

fppd_log_since() {
    local offset="$1"
    docker exec "$CONTAINER" sh -c "tail -n +$((offset + 1)) $FPPD_LOG 2>/dev/null || true"
}

# Emitted only after the callbacks declaration file was found and is about to
# be run; absent entirely when it is missing. Verified present in both pinned
# trees (FPP 9 src/Plugins.cpp:445, FPP 10 src/Plugins.cpp:558).
PLUGIN_LOAD_OK_LINE="Processing Callbacks (${PLUGIN_DIR}/callbacks) for plugin: 'fpp-showmesh'"
# Every load-path failure string in both pinned trees' Plugins.cpp.
PLUGIN_LOAD_FAIL_RE="Failed to load plugin|Failed to find shlib|Failed to load shlib|Failed to find  createPlugin|Failed to create plugin from shlib|Could not load plugin"

command_json() {
    curl -fsS "${API}/commands" 2>/dev/null | jq -c --arg name "$COMMAND_NAME" '.[] | select(.name == $name)' 2>/dev/null || true
}

# curl exits nonzero on a connection-level failure (observed repeatedly as
# transient "Recv failure: Connection reset by peer" against this emulated
# container), which under `set -e` would kill the run at a variable
# assignment. Retries, and emits ONLY the last attempt's stdout so a
# -w '%{http_code}' caller never sees concatenated codes like 000000200.
# Returns 0 even on total failure: the emitted code is then the last failed
# attempt's (typically 000), which fails every caller's "200" comparison, so
# the outcome is a normal FAIL rather than an abort.
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

# ---------------------------------------------------------------------------
# A1: fppd loads the plugin (positive evidence, plus a negative control)
# ---------------------------------------------------------------------------

a1() {
    # Negative control first, so the positive evidence is read from the tail
    # of a restart that this assertion performed itself.
    local absent_offset
    absent_offset="$(fppd_log_line_count)"
    docker exec "$CONTAINER" mv "${PLUGIN_DIR}/callbacks" "${PLUGIN_DIR}/callbacks.disabled"
    restart_and_wait
    local absent_json absent_tail
    absent_json="$(command_json)"
    absent_tail="$(fppd_log_since "$absent_offset")"

    local present_offset
    present_offset="$(fppd_log_line_count)"
    docker exec "$CONTAINER" mv "${PLUGIN_DIR}/callbacks.disabled" "${PLUGIN_DIR}/callbacks"
    restart_and_wait
    local present_json present_tail
    present_json="$(command_json)"
    present_tail="$(fppd_log_since "$present_offset")"

    if [ -n "$absent_json" ]; then
        record "A1_loads_plugin" "FAIL" "command still present with the callbacks file removed: $absent_json"
        return
    fi
    if echo "$absent_tail" | grep -qF "$PLUGIN_LOAD_OK_LINE"; then
        record "A1_loads_plugin" "FAIL" "the load-success log line appeared in the negative control, so it does not discriminate: $PLUGIN_LOAD_OK_LINE"
        return
    fi
    if [ -z "$present_json" ]; then
        record "A1_loads_plugin" "FAIL" "command '$COMMAND_NAME' absent from GET /api/commands with the callbacks file in place"
        return
    fi
    if ! echo "$present_tail" | grep -qF "$PLUGIN_LOAD_OK_LINE"; then
        record "A1_loads_plugin" "FAIL" "expected load line missing from the fppd.log tail written by this restart: $PLUGIN_LOAD_OK_LINE"
        return
    fi
    local load_errs
    load_errs="$(echo "$present_tail" | grep -E "$PLUGIN_LOAD_FAIL_RE" || true)"
    if [ -n "$load_errs" ]; then
        record "A1_loads_plugin" "FAIL" "load-failure line in the fppd.log tail written by this restart: $(echo "$load_errs" | head -3 | tr '\n' ' ')"
        return
    fi
    record "A1_loads_plugin" "PASS" "command registered with the callbacks file in place and absent without it; this restart's log tail carries the load line and no load-failure line"
}

# ---------------------------------------------------------------------------
# A2: registered command vocabulary
# ---------------------------------------------------------------------------

a2() {
    local json
    json="$(command_json)"
    if [ -z "$json" ]; then
        record "A2_command_vocabulary" "FAIL" "command absent from GET /api/commands"
        return
    fi

    local target_name target_min target_max fade_name fade_min fade_max fade_default
    target_name="$(echo "$json" | jq -r '.args[0].name // "MISSING"')"
    target_min="$(echo "$json" | jq -r '.args[0].min // "MISSING"')"
    target_max="$(echo "$json" | jq -r '.args[0].max // "MISSING"')"
    fade_name="$(echo "$json" | jq -r '.args[1].name // "MISSING"')"
    fade_min="$(echo "$json" | jq -r '.args[1].min // "MISSING"')"
    fade_max="$(echo "$json" | jq -r '.args[1].max // "MISSING"')"
    fade_default="$(echo "$json" | jq -r '.args[1].default // "MISSING"')"

    local errs=""
    [ "$target_name" = "targetPercent" ] || errs="${errs} args[0].name=$target_name"
    [ "$target_min" = "0" ] || errs="${errs} targetPercent.min=$target_min"
    [ "$target_max" = "100" ] || errs="${errs} targetPercent.max=$target_max"
    [ "$fade_name" = "fadeSeconds" ] || errs="${errs} args[1].name=$fade_name"
    [ "$fade_min" = "0" ] || errs="${errs} fadeSeconds.min=$fade_min"
    [ "$fade_max" = "86400" ] || errs="${errs} fadeSeconds.max=$fade_max"
    [ "$fade_default" = "0" ] || errs="${errs} fadeSeconds.default=$fade_default"

    if [ -n "$errs" ]; then
        record "A2_command_vocabulary" "FAIL" "mismatches:$errs (full: $json)"
        return
    fi
    record "A2_command_vocabulary" "PASS" "targetPercent min=$target_min max=$target_max, fadeSeconds min=$fade_min max=$fade_max default=$fade_default"
}

# ---------------------------------------------------------------------------
# A3: invoke, range enforcement, and OUR error text (never "FPP rejects it")
# ---------------------------------------------------------------------------

a3() {
    local code body

    code="$(invoke_command 50 0)"; body="$(invoke_command_body)"
    if [ "$code" != "200" ] || ! echo "$body" | grep -qi "brightness ceiling set to 50 percent"; then
        record "A3_invoke_and_ranges" "FAIL" "in-range invoke: http=$code body=$body"
        return
    fi

    code="$(invoke_command 150 0)"; body="$(invoke_command_body)"
    if [ "$code" != "500" ] || ! echo "$body" | grep -qF "target percent must be between 0 and 100"; then
        record "A3_invoke_and_ranges" "FAIL" "out-of-range targetPercent expected our own refusal text: http=$code body=$body"
        return
    fi

    code="$(invoke_command banana 0)"; body="$(invoke_command_body)"
    if [ "$code" != "500" ] || ! echo "$body" | grep -qF "target percent must be a whole number between 0 and 100"; then
        record "A3_invoke_and_ranges" "FAIL" "non-integer targetPercent expected our own refusal text: http=$code body=$body"
        return
    fi

    code="$(invoke_command 50 99999)"; body="$(invoke_command_body)"
    if [ "$code" != "500" ] || ! echo "$body" | grep -qF "fade seconds must be between 0 and 86400"; then
        record "A3_invoke_and_ranges" "FAIL" "out-of-range fadeSeconds expected our own refusal text: http=$code body=$body"
        return
    fi

    record "A3_invoke_and_ranges" "PASS" "in-range invoke returned 200; out-of-range targetPercent, non-integer targetPercent and out-of-range fadeSeconds each returned 500 carrying our own refusal text"
}

# ---------------------------------------------------------------------------
# A4 / A5: real channel output downstream of modifyChannelData
#
# The observable is a DDP datagram on the wire, which is the only thing
# available downstream of modifyChannelData: the overlay-model API,
# /api/channel/* and /api/fppd/testing all read or write upstream of the hook.
# The output-processor chain is left empty so the wire bytes are a byte-exact
# readback of what the plugin wrote.
# ---------------------------------------------------------------------------

A4_NAME="A4_channel_output_scaled"
A5_NAME="A5_fade_monotonic_exact_and_immediate"

CO_UNIVERSES_START_CHANNEL=1
CO_UNIVERSES_COUNT=48
DDP_PORT=4048
# A fade over FADE_SECONDS at the observed loopback DDP frame rate yields far
# more than this; a capture with fewer samples is not a fade measurement.
A5_MIN_SAMPLES=20
A5_FADE_SECONDS=4

# The plugin scales with (value * percent + 50) / 100 in integer arithmetic
# (native/src/brightness.cpp), and returns early leaving the byte untouched at
# 100 percent. This mirrors it so the expected wire byte is derived, not
# hardcoded from an observation.
scaled_byte_hex() {
    local percent="$1"
    printf '%02x' $(( (255 * percent + 50) / 100 ))
}

# Schema verified in BOTH pinned trees: ChannelOutputSetup.cpp reads
# root["channelOutputs"], remaps the string type "universes" to the UDPOutput
# plugin (there is no separate DDP shared object in either build), and
# UDPOutputData reads the per-universe numeric "type" (4 = DDP) and "address".
configure_ddp_output() {
    local json
    json=$(cat <<JSON
{
  "channelOutputs": [
    {
      "type": "universes",
      "enabled": 1,
      "startChannel": ${CO_UNIVERSES_START_CHANNEL},
      "channelCount": ${CO_UNIVERSES_COUNT},
      "universes": [
        {
          "active": 1,
          "type": 4,
          "startChannel": ${CO_UNIVERSES_START_CHANNEL},
          "channelCount": ${CO_UNIVERSES_COUNT},
          "address": "127.0.0.1",
          "description": "showmesh-bench-ddp"
        }
      ]
    }
  ]
}
JSON
)
    echo "$json" | docker exec -i "$CONTAINER" sh -c "cat > /home/fpp/media/config/co-universes.json"
}

# FPP's own Channel Output Testing writes into the pre-plugin buffer, so this
# is the known upstream value the plugin's scaling is measured against. Body
# shape verified in BOTH pinned trees: ChannelTester.cpp's render_POST hands
# the body to SetupTest, and TestPatternBase/RGBFill read "enabled", "mode",
# "channelSet" and the color keys used here.
set_static_test_pattern() {
    local value="$1"
    curl_retrying -sS -o /dev/null -w '%{http_code}' \
        -H 'Content-Type: application/json' \
        -d "{\"enabled\":1,\"mode\":\"RGBFill\",\"channelSet\":\"${CO_UNIVERSES_START_CHANNEL}-$((CO_UNIVERSES_START_CHANNEL + CO_UNIVERSES_COUNT - 1))\",\"channelsPerNode\":3,\"color1\":${value},\"color2\":${value},\"color3\":${value},\"color4\":0}" \
        "${API}/fppd/testing"
}

CAPTURE_SEQ=0
CAPTURE_LOG=""
# 15 chars exactly: /proc/[pid]/comm is truncated to 15 chars on Linux.
# Verified empirically that `sh -c "exec -a NAME python3 ..."` does NOT
# change comm (comm is set from the executed file's own path, not argv[0],
# so it still reads "python3"); a symlink named CAPTURE_PROC_NAME pointing at
# the real python3 binary, executed directly, does change comm, confirmed by
# reading /proc/[pid]/comm and by pgrep -x matching it.
CAPTURE_PROC_NAME="showmesh-ddpcap"
CAPTURE_BIN="/tmp/${CAPTURE_PROC_NAME}"

# `docker exec -d` returns 0 whether or not the listener survived, so the
# listener's bind must be confirmed out of band and the previous listener must
# be gone before the next one binds the same port. Matched by the comm name
# of the CAPTURE_BIN symlink below, not the bare "python3" comm, so an
# unrelated python3 process in the container (fppd helper, etc) is never
# touched.
stop_capture() {
    docker exec "$CONTAINER" pkill -x "$CAPTURE_PROC_NAME" >/dev/null 2>&1 || true
    local i=0
    while [ "$i" -lt 30 ]; do
        if ! docker exec "$CONTAINER" pgrep -x "$CAPTURE_PROC_NAME" >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

# Returns nonzero unless the listener actually bound; each capture gets its own
# log path so no live listener's file is ever unlinked underneath it.
start_capture() {
    local duration="$1"
    if ! stop_capture; then
        echo "test-plugin-load-fpp: a previous DDP capture listener would not exit" >&2
        return 1
    fi
    CAPTURE_SEQ=$((CAPTURE_SEQ + 1))
    CAPTURE_LOG="/tmp/showmesh-ddp-capture.${CAPTURE_SEQ}.log"
    local capture_script="/tmp/showmesh-ddp-capture.${CAPTURE_SEQ}.py"
    docker exec "$CONTAINER" rm -f "$CAPTURE_LOG" "${CAPTURE_LOG}.ready" "${CAPTURE_LOG}.done" "$capture_script"
    docker exec -i "$CONTAINER" sh -c "cat > $capture_script" <<PYEOF
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', ${DDP_PORT}))
s.settimeout(0.5)
f = open('${CAPTURE_LOG}', 'w')
open('${CAPTURE_LOG}.ready', 'w').close()
end = time.time() + ${duration}
while time.time() < end:
    try:
        data, _ = s.recvfrom(65535)
    except socket.timeout:
        continue
    f.write(str(time.time()) + ' ' + data.hex() + chr(10))
    f.flush()
f.close()
s.close()
open('${CAPTURE_LOG}.done', 'w').close()
PYEOF
    # A symlink named CAPTURE_PROC_NAME to the real python3, executed
    # directly, reports that name as its own comm, so stop_capture can target
    # only this listener and never a co-resident python3 process.
    docker exec "$CONTAINER" sh -c "ln -sf \"\$(command -v python3)\" '$CAPTURE_BIN'"
    docker exec -d "$CONTAINER" "$CAPTURE_BIN" "$capture_script"
    local i=0
    while [ "$i" -lt 60 ]; do
        if docker exec "$CONTAINER" test -f "${CAPTURE_LOG}.ready"; then
            return 0
        fi
        i=$((i + 1))
        sleep 0.5
    done
    echo "test-plugin-load-fpp: the DDP capture listener never signalled a successful bind on port ${DDP_PORT}" >&2
    return 1
}

wait_capture() {
    local i=0
    while [ "$i" -lt 120 ]; do
        if docker exec "$CONTAINER" test -f "${CAPTURE_LOG}.done"; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

read_capture() {
    docker exec "$CONTAINER" cat "$CAPTURE_LOG" 2>/dev/null || true
}

# A DDP datagram is a fixed 10-byte header then channel data, so the payload
# starts at hex offset 20.
ddp_payload_hex() {
    local hex="${1#* }"
    echo "${hex:20}"
}

# Emits one lowercase hex byte per captured datagram, taken from the first
# channel of the payload.
capture_first_bytes() {
    local capture="$1" line payload
    while IFS= read -r line; do
        if [ -z "$line" ]; then
            continue
        fi
        payload="$(ddp_payload_hex "$line")"
        if [ -z "$payload" ]; then
            continue
        fi
        echo "${payload:0:2}" | tr '[:upper:]' '[:lower:]'
    done <<< "$capture"
}

a4_a5() {
    if ! docker exec "$CONTAINER" sh -c 'command -v python3 >/dev/null 2>&1'; then
        record "$A4_NAME" "FAIL" "python3 not available in the container to capture DDP"
        record "$A5_NAME" "FAIL" "python3 not available in the container to capture DDP"
        return
    fi

    configure_ddp_output
    restart_and_wait

    local test_pattern_code
    test_pattern_code="$(set_static_test_pattern 255)"
    if [ "$test_pattern_code" != "200" ]; then
        record "$A4_NAME" "FAIL" "could not set the known upstream channel value (POST /api/fppd/testing returned $test_pattern_code)"
        record "$A5_NAME" "FAIL" "same blocker as A4: no known upstream channel value to observe scaling against"
        return
    fi

    a4_scaling
    a5_fade
}

a4_scaling() {
    local want100 want50
    want100="$(scaled_byte_hex 100)"
    want50="$(scaled_byte_hex 50)"

    invoke_command 100 0 >/dev/null; invoke_command_body >/dev/null
    if ! start_capture 3; then
        record "$A4_NAME" "FAIL" "could not start the DDP capture listener at ceiling 100"
        return
    fi
    wait_capture || true
    local cap100
    cap100="$(read_capture)"

    invoke_command 50 0 >/dev/null; invoke_command_body >/dev/null
    if ! start_capture 3; then
        record "$A4_NAME" "FAIL" "could not start the DDP capture listener at ceiling 50"
        return
    fi
    wait_capture || true
    local cap50
    cap50="$(read_capture)"

    if [ -z "$cap100" ] || [ -z "$cap50" ]; then
        record "$A4_NAME" "FAIL" "no DDP datagrams captured (empty capture at ceiling 100 and/or 50)"
        return
    fi

    local p100 p50 b100 b50
    p100="$(ddp_payload_hex "$(echo "$cap100" | tail -1)")"
    p50="$(ddp_payload_hex "$(echo "$cap50" | tail -1)")"
    if [ -z "$p100" ] || [ -z "$p50" ]; then
        record "$A4_NAME" "FAIL" "captured datagrams carry no readable payload: 100='$p100' 50='$p50'"
        return
    fi
    if [ "$p100" = "$p50" ]; then
        record "$A4_NAME" "FAIL" "ceiling 100 and ceiling 50 produced identical wire payloads, so the plugin did not scale the data: $p100"
        return
    fi
    b100="$(echo "${p100:0:2}" | tr '[:upper:]' '[:lower:]')"
    b50="$(echo "${p50:0:2}" | tr '[:upper:]' '[:lower:]')"
    if [ "$b100" != "$want100" ] || [ "$b50" != "$want50" ]; then
        record "$A4_NAME" "FAIL" "wire byte for a 0xff source channel was 0x$b100 at ceiling 100 (expected 0x$want100) and 0x$b50 at ceiling 50 (expected 0x$want50)"
        return
    fi
    record "$A4_NAME" "PASS" "0xff source scaled to 0x$b100 at ceiling 100 and 0x$b50 at ceiling 50"
}

a5_fade() {
    local want_final want_full want_quarter
    want_final="$(scaled_byte_hex 75)"
    want_full="$(scaled_byte_hex 100)"
    want_quarter="$(scaled_byte_hex 25)"

    # Part 1: a 100 to 75 fade, sampled across the whole fade window.
    invoke_command 100 0 >/dev/null; invoke_command_body >/dev/null
    if ! start_capture $((A5_FADE_SECONDS + 3)); then
        record "$A5_NAME" "FAIL" "could not start the DDP capture listener for the fade window"
        return
    fi
    invoke_command 75 "$A5_FADE_SECONDS" >/dev/null; invoke_command_body >/dev/null
    if ! wait_capture; then
        record "$A5_NAME" "FAIL" "the fade-window DDP capture never finished"
        return
    fi
    local capfade bytes count first last
    capfade="$(read_capture)"
    if [ -z "$capfade" ]; then
        record "$A5_NAME" "FAIL" "no DDP datagrams captured during the fade window"
        return
    fi
    bytes="$(capture_first_bytes "$capfade")"
    count="$(echo "$bytes" | grep -c . || true)"
    if [ "$count" -lt "$A5_MIN_SAMPLES" ]; then
        record "$A5_NAME" "FAIL" "only $count usable frames sampled across the fade window, fewer than the $A5_MIN_SAMPLES required to call this a fade measurement"
        return
    fi
    first="$(echo "$bytes" | head -1)"
    last="$(echo "$bytes" | tail -1)"
    # Strictly decreasing overall: an inert plugin holds one constant value
    # and fails here.
    if [ "$((16#$first))" -le "$((16#$last))" ]; then
        record "$A5_NAME" "FAIL" "the sampled channel byte did not decrease across the fade window: first 0x$first, last 0x$last over $count frames"
        return
    fi
    local prev="" b
    while IFS= read -r b; do
        if [ -n "$prev" ] && [ "$((16#$b))" -gt "$((16#$prev))" ]; then
            record "$A5_NAME" "FAIL" "the sampled channel byte increased mid-fade: 0x$prev then 0x$b"
            return
        fi
        prev="$b"
    done <<< "$bytes"
    if [ "$last" != "$want_final" ]; then
        record "$A5_NAME" "FAIL" "the fade did not land on the exact 75 percent value: final wire byte 0x$last, expected 0x$want_final"
        return
    fi

    # Part 2: fadeSeconds 0 applies immediately, so the capture must contain
    # only the before and after values and no intermediate step.
    invoke_command 100 0 >/dev/null; invoke_command_body >/dev/null
    if ! start_capture 6; then
        record "$A5_NAME" "FAIL" "could not start the DDP capture listener for the zero-seconds case"
        return
    fi
    sleep 2
    invoke_command 25 0 >/dev/null; invoke_command_body >/dev/null
    if ! wait_capture; then
        record "$A5_NAME" "FAIL" "the zero-seconds DDP capture never finished"
        return
    fi
    local capzero zbytes distinct unexpected
    capzero="$(read_capture)"
    if [ -z "$capzero" ]; then
        record "$A5_NAME" "FAIL" "no DDP datagrams captured during the zero-seconds window"
        return
    fi
    zbytes="$(capture_first_bytes "$capzero")"
    distinct="$(echo "$zbytes" | sort -u | tr '\n' ' ')"
    unexpected="$(echo "$zbytes" | sort -u | grep -v -x -e "$want_full" -e "$want_quarter" || true)"
    if [ -n "$unexpected" ]; then
        record "$A5_NAME" "FAIL" "fadeSeconds 0 produced intermediate wire values instead of applying at once: saw $distinct, expected only 0x$want_full and 0x$want_quarter"
        return
    fi
    if ! echo "$zbytes" | grep -qx "$want_full"; then
        record "$A5_NAME" "FAIL" "fadeSeconds 0 window never showed the pre-command value 0x$want_full: saw $distinct"
        return
    fi
    if ! echo "$zbytes" | grep -qx "$want_quarter"; then
        record "$A5_NAME" "FAIL" "fadeSeconds 0 did not apply within the capture window: never saw 0x$want_quarter, saw $distinct"
        return
    fi

    invoke_command 100 0 >/dev/null; invoke_command_body >/dev/null
    record "$A5_NAME" "PASS" "$count frames, 0x$first down to 0x$last (exact 75 percent value), non-increasing throughout; fadeSeconds 0 stepped straight from 0x$want_full to 0x$want_quarter"
}

# ---------------------------------------------------------------------------
# A6: clean teardown
# ---------------------------------------------------------------------------

# `pkill -f`/`pgrep -f` cannot be used here: the pattern would appear in the
# invoking shell's own argv, so pkill SIGTERMs its own ancestor and pgrep
# always self-matches. Matching the process NAME with -x, with no `sh -c`
# wrapper, is what makes A6 able to fail and able to pass.
stop_fppd_and_check() {
    local phase="$1" offset
    offset="$(fppd_log_line_count)"
    docker exec "$CONTAINER" pkill -x fppd >/dev/null 2>&1 || true

    local i=0 exited=0
    while [ "$i" -lt 30 ]; do
        if ! docker exec "$CONTAINER" pgrep -x fppd >/dev/null 2>&1; then
            exited=1
            break
        fi
        i=$((i + 1))
        sleep 1
    done
    if [ "$exited" != "1" ]; then
        record "A6_clean_teardown" "FAIL" "fppd did not exit within 30s of SIGTERM ($phase)"
        restart_and_wait
        return 1
    fi

    # Let the crash handler's own output finish landing in the log.
    sleep 2
    # Only the tail written after the stop signal: fppd.log persists across
    # restarts and across runs sharing this --id's volume, so a stale crash
    # would otherwise read as a fresh one, and a long backtrace would fall out
    # of any fixed-size tail window.
    local crash
    crash="$(fppd_log_since "$offset" | grep -iE "terminate called|SIGSEGV|SIGABRT|core dumped|Crash handler called" || true)"
    if [ -n "$crash" ]; then
        record "A6_clean_teardown" "FAIL" "crash or terminate signature written to fppd.log after the stop signal ($phase): $(echo "$crash" | head -3 | tr '\n' ' | ')"
        restart_and_wait
        return 1
    fi
    return 0
}

a6() {
    # Order 1, on both majors: shut down with the plugin still resident. This
    # is the teardown path a real host takes.
    if ! stop_fppd_and_check "with the plugin loaded"; then
        return
    fi

    local unload_note="n/a on FPP 9: no runtime unload endpoint exists"
    if [ "$BENCH_FPP_MAJOR" = "fpp10" ]; then
        restart_and_wait
        # The unload response by itself is NOT evidence: PluginManager::unloadPlugin
        # returns true when findPluginByDir misses, so the endpoint answers 200
        # with loaded:false for a name that was never loaded at all. Confirmed
        # against a running container, where an unload of "does-not-exist"
        # returned exactly that. So the real observable is the plugin's own
        # registered command being present before and gone after.
        local before_unload
        before_unload="$(command_json)"
        if [ -z "$before_unload" ]; then
            record "A6_clean_teardown" "FAIL" "the plugin's command was already absent from GET /api/commands before the unload, so the unload could not be observed at all"
            restart_and_wait
            return
        fi
        local unload_code unload_body loaded_field
        unload_code=$(curl_retrying -sS -o "/tmp/showmesh-bench-unload.$$" -w '%{http_code}' -X POST "${API}/fppd/plugin/fpp-showmesh/unload")
        unload_body="$(cat "/tmp/showmesh-bench-unload.$$" 2>/dev/null || true)"
        rm -f "/tmp/showmesh-bench-unload.$$" 2>/dev/null || true
        # has("loaded") before tostring: jq's `//` treats JSON false as falsy,
        # so `.loaded // "MISSING"` would print MISSING for the exact response
        # being confirmed.
        loaded_field="$(echo "$unload_body" | jq -r 'if has("loaded") then (.loaded | tostring) else "MISSING" end' 2>/dev/null || echo MISSING)"
        if [ "$unload_code" != "200" ] || [ "$loaded_field" != "false" ]; then
            record "A6_clean_teardown" "FAIL" "POST .../fpp-showmesh/unload expected 200 with loaded:false, got http=$unload_code body=$unload_body"
            restart_and_wait
            return
        fi
        local after_unload
        after_unload="$(command_json)"
        if [ -n "$after_unload" ]; then
            record "A6_clean_teardown" "FAIL" "the unload answered loaded:false but the plugin's command is still registered, so nothing was actually unloaded: $after_unload"
            restart_and_wait
            return
        fi
        # Order 2, FPP 10 only: shut down again after the runtime unload.
        if ! stop_fppd_and_check "after a runtime unload"; then
            return
        fi
        unload_note="the unload withdrew the plugin's registered command (present before, absent after) and answered loaded:false, and the post-unload shutdown was also clean: $unload_body"
    fi

    record "A6_clean_teardown" "PASS" "fppd exited within 30s and wrote no crash signature after the stop signal; $unload_note"
    restart_and_wait
}

# ---------------------------------------------------------------------------
# A7: no second listener on UDP 32320, and the one listener is fppd
# ---------------------------------------------------------------------------

# `ss -H -lunp` names the holding process, which /proc/net/udp does not, so
# "some other process bound 32320 because fppd failed to" cannot pass here.
sockets_on_32320() {
    docker exec "$CONTAINER" ss -H -lunp 2>/dev/null | awk '$4 ~ /:32320$/' || true
}

check_32320() {
    local phase="$1" lines="$2" count
    count="$(echo "$lines" | grep -c . || true)"
    if [ "$count" != "1" ]; then
        echo "expected exactly 1 socket bound to UDP 32320 $phase, found $count: $(echo "$lines" | tr '\n' ' | ')"
        return 1
    fi
    if ! echo "$lines" | grep -q '"fppd"'; then
        echo "the single UDP 32320 listener $phase is not fppd: $lines"
        return 1
    fi
    return 0
}

a7() {
    local before after detail
    before="$(sockets_on_32320)"
    # The plugin is loaded throughout this run, so the honest before/after this
    # bench can produce is across a restart with the same install in place.
    restart_and_wait
    after="$(sockets_on_32320)"

    if ! detail="$(check_32320 "before the restart" "$before")"; then
        record "A7_no_second_multisync_listener" "FAIL" "$detail"
        return
    fi
    if ! detail="$(check_32320 "after the restart" "$after")"; then
        record "A7_no_second_multisync_listener" "FAIL" "$detail"
        return
    fi
    record "A7_no_second_multisync_listener" "PASS" "exactly one UDP 32320 socket, held by fppd, before and after a restart"
}

# ---------------------------------------------------------------------------
# A8: restart survival
# ---------------------------------------------------------------------------

a8() {
    local offset
    offset="$(fppd_log_line_count)"
    # Same --force-recreate path as everywhere else, so A8 is a real test of
    # "does the plugin load again" rather than one that dodges the image's
    # apache stale-pid crash-loop by restarting differently.
    "${COMPOSE[@]}" up -d --force-recreate >/dev/null
    if ! wait_for_http 90; then
        record "A8_restart_survival" "FAIL" "fppd did not come back up after a container restart"
        return
    fi
    local json tail_log load_errs
    json="$(command_json)"
    tail_log="$(fppd_log_since "$offset")"
    if [ -z "$json" ]; then
        record "A8_restart_survival" "FAIL" "command absent from GET /api/commands after container restart"
        return
    fi
    if ! echo "$tail_log" | grep -qF "$PLUGIN_LOAD_OK_LINE"; then
        record "A8_restart_survival" "FAIL" "expected load line missing from the fppd.log tail written by the restart: $PLUGIN_LOAD_OK_LINE"
        return
    fi
    load_errs="$(echo "$tail_log" | grep -E "$PLUGIN_LOAD_FAIL_RE" || true)"
    if [ -n "$load_errs" ]; then
        record "A8_restart_survival" "FAIL" "load-failure line in the fppd.log tail written by the restart: $(echo "$load_errs" | head -3 | tr '\n' ' ')"
        return
    fi
    record "A8_restart_survival" "PASS" "command present again after a container recreate, and the restart's own log tail carries the load line with no load-failure line"
}

# ---------------------------------------------------------------------------
# Run all assertions, then report
# ---------------------------------------------------------------------------

a1
a2
a3
a4_a5
a6
a7
a8

print_summary
if [ "$SUMMARY_FAILED" -ne 0 ]; then
    exit 1
fi
exit 0
