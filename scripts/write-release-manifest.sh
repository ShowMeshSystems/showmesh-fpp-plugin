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
pin_field() {
    local major="$1" column="$2"
    awk -F'|' -v major="$major" -v col="$column" '
        $0 ~ ("^\\| " major " ") {
            gsub(/[ `]/, "", $col)
            print $col
            exit
        }' "$pins"
}

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
printf '    "fpp9": { "tag": "%s", "commit": "%s" },\n' "$(pin_field 'FPP 9' 3)" "$(pin_field 'FPP 9' 4)"
printf '    "fpp10": { "tag": "%s", "commit": "%s" }\n' "$(pin_field 'FPP 10' 3)" "$(pin_field 'FPP 10' 4)"
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
