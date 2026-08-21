#!/usr/bin/env bash
# Writes the release manifest for the artifacts already built into a
# directory. Everything it emits is derived from those files, so it cannot
# describe an artifact that was not produced, and it carries no timestamp,
# so two builds of one commit produce the same manifest.
set -euo pipefail

dist="${1:?usage: write-release-manifest.sh <dist-dir> <version> <commit>}"
version="${2:?missing version}"
commit="${3:?missing commit}"

pins="native/adapters/FPP-PINS.md"
[ -f "$pins" ] || { echo "write-release-manifest: $pins does not exist" >&2; exit 1; }

sha_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

size_of() {
    # -c is bytes on BSD stat and total size on GNU stat, so ask each in
    # its own dialect rather than guessing.
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

# Reads one column out of the pinned-version table by its FPP major.
# Counts matching rows at END rather than exiting on the first, so a
# newer row accidentally added above an existing one for the same major
# fails loudly instead of silently winning by table position.
pin_field() {
    local major="$1" column="$2"
    awk -F'|' -v major="$major" -v col="$column" '
        $0 ~ ("^\\| " major " ") {
            gsub(/[ `]/, "", $col)
            print $col
            n++
        }
        END { exit (n == 1 ? 0 : 1) }' "$pins"
}

# Runs pin_field and fails the whole script if the lookup did not resolve
# to exactly one row, or resolved to a value that is not a plausible pin.
# pin_field runs inside `printf`'s own argument command substitution
# below, where `set -e` cannot see a failure, so every lookup is resolved
# to a variable and checked here instead.
fpp_tag() {
    local major="$1" value
    if ! value=$(pin_field "$major" 3); then
        echo "write-release-manifest: $pins has no exactly-one row for major '$major'" >&2
        exit 1
    fi
    [ -n "$value" ] || { echo "write-release-manifest: $pins has an empty tag for major '$major'" >&2; exit 1; }
    printf '%s' "$value"
}

fpp_commit() {
    local major="$1" value
    if ! value=$(pin_field "$major" 4); then
        echo "write-release-manifest: $pins has no exactly-one row for major '$major'" >&2
        exit 1
    fi
    if [[ ! "$value" =~ ^[0-9a-f]{40}$ ]]; then
        echo "write-release-manifest: $pins has a commit '$value' for major '$major' that is not exactly 40 hex characters" >&2
        exit 1
    fi
    printf '%s' "$value"
}

fpp9_tag=$(fpp_tag 'FPP 9')
fpp9_commit=$(fpp_commit 'FPP 9')
fpp10_tag=$(fpp_tag 'FPP 10')
fpp10_commit=$(fpp_commit 'FPP 10')

artifact_entry() {
    local file="$1" arch="$2" kind="$3"
    local path="$dist/$file"
    [ -f "$path" ] || { echo "missing artifact: $path" >&2; exit 1; }
    printf '    {\n'
    printf '      "filename": "%s",\n' "$file"
    printf '      "kind": "%s",\n' "$kind"
    printf '      "architecture": "%s",\n' "$arch"
    printf '      "sizeBytes": %s,\n' "$(size_of "$path")"
    printf '      "sha256": "%s"\n' "$(sha_of "$path")"
    printf '    }'
}

printf '{\n'
printf '  "manifestVersion": 1,\n'
printf '  "version": "%s",\n' "$version"
printf '  "sourceCommit": "%s",\n' "$commit"
printf '  "fppSupport": {\n'
printf '    "fpp9": { "tag": "%s", "commit": "%s" },\n' "$fpp9_tag" "$fpp9_commit"
printf '    "fpp10": { "tag": "%s", "commit": "%s" }\n' "$fpp10_tag" "$fpp10_commit"
printf '  },\n'
printf '  "artifacts": [\n'

first=1
emit() {
    [ $first -eq 1 ] || printf ',\n'
    first=0
    artifact_entry "$@"
}

emit "showmesh-fpp-plugin_${version}_linux_amd64.tar.gz" amd64 "go-helper"
emit "showmesh-fpp-plugin_${version}_linux_arm64.tar.gz" arm64 "go-helper"
emit "showmesh-fpp-plugin_${version}_linux_armv7.tar.gz" armv7 "go-helper"
emit "showmesh-fpp-plugin-native_${version}.tar.gz" any "native-source"
printf '\n  ]\n'
printf '}\n'
