#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
mvcc_dir="${1:-upstream/mvcc_tm}"
patch_file="${2:-third_party/mvcc_tm-lincheck-hooks.patch}"

mvcc_path="$repo_root/$mvcc_dir"
patch_path="$repo_root/$patch_file"

if [[ ! -d "$mvcc_path/.git" ]]; then
    echo "missing Multiverse checkout: $mvcc_path" >&2
    exit 1
fi

if [[ ! -f "$patch_path" ]]; then
    echo "missing Multiverse patch: $patch_path" >&2
    exit 1
fi

git -C "$mvcc_path" apply --unidiff-zero --reverse --check "$patch_path"
git -C "$mvcc_path" diff --unified=0 -- multiverse/multiverse.hpp | diff -u "$patch_path" -

echo "Multiverse patch is reversible and matches the checkout diff"
