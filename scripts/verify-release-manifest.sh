#!/usr/bin/env bash
# Re-derives every hash and size in the manifest from the artifacts on
# disk and fails on any disagreement. The manifest is what a packaging
# repository commits as its trust anchor, so it is verified where it is
# produced rather than trusted because it was just written.
set -euo pipefail

dist="${1:?usage: verify-release-manifest.sh <dist-dir> <version>}"
version="${2:?missing version}"
manifest="$dist/release-manifest.json"

[ -f "$manifest" ] || { echo "verify-release-manifest: $manifest does not exist" >&2; exit 1; }

sha_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

size_of() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; }

# The manifest is written one field per line, so this reads it without a
# JSON parser, which an FPP host and a bare CI runner both may lack.
filenames=$(grep '"filename"' "$manifest" | sed 's/.*: "//; s/".*//')
[ -n "$filenames" ] || { echo "verify-release-manifest: the manifest names no artifacts" >&2; exit 1; }

count=0
while IFS= read -r filename; do
    [ -n "$filename" ] || continue
    path="$dist/$filename"
    if [ ! -f "$path" ]; then
        echo "verify-release-manifest: the manifest names $filename, which was not built" >&2
        exit 1
    fi

    recorded_sha=$(awk -v f="\"$filename\"" '
        $0 ~ f { found = 1 }
        found && /"sha256"/ { sub(/.*: "/, ""); sub(/".*/, ""); print; exit }' "$manifest")
    recorded_size=$(awk -v f="\"$filename\"" '
        $0 ~ f { found = 1 }
        found && /"sizeBytes"/ { sub(/.*: /, ""); sub(/,.*/, ""); print; exit }' "$manifest")

    actual_sha=$(sha_of "$path")
    actual_size=$(size_of "$path")

    if [ "$recorded_sha" != "$actual_sha" ]; then
        echo "verify-release-manifest: $filename hashes $actual_sha, manifest records $recorded_sha" >&2
        exit 1
    fi
    if [ "$recorded_size" != "$actual_size" ]; then
        echo "verify-release-manifest: $filename is $actual_size bytes, manifest records $recorded_size" >&2
        exit 1
    fi
    count=$((count + 1))
done <<EOF
$filenames
EOF

# Every artifact the checksums file covers must also appear in the
# manifest. Otherwise a build could ship a file the packaging repository
# never pins.
sums="$dist/showmesh-fpp-plugin_${version}_SHA256SUMS"
if [ -f "$sums" ]; then
    while read -r _ name; do
        [ -n "$name" ] || continue
        if ! grep -q "\"$name\"" "$manifest"; then
            echo "verify-release-manifest: $name is in the checksums file but not in the manifest" >&2
            exit 1
        fi
    done < "$sums"
fi

echo "verify-release-manifest: OK, re-derived $count artifact hashes and sizes"
