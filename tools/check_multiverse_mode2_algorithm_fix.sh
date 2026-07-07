#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
build_dir="${1:-build}"
mvcc_dir="${2:-upstream/mvcc_tm}"
patch_file="${3:-third_party/mvcc_tm-mode2-algorithm-fixes.patch}"

build_path="$repo_root/$build_dir"
mvcc_path="$repo_root/$mvcc_dir"
patch_path="$repo_root/$patch_file"
schedule_file="$(mktemp)"
patch_applied=0

cleanup() {
    status=$?
    if [[ "$patch_applied" -eq 1 ]]; then
        if ! git -C "$mvcc_path" apply --reverse "$patch_path"; then
            status=1
        fi
        if ! cmake --build "$build_path" --target multiverse_smoke_tests -j2 >/dev/null; then
            status=1
        fi
    fi
    rm -f "$schedule_file"
    exit "$status"
}
trap cleanup EXIT

if [[ ! -d "$build_path" ]]; then
    echo "missing build directory: $build_path" >&2
    exit 1
fi

if [[ ! -d "$mvcc_path/.git" ]]; then
    echo "missing Multiverse checkout: $mvcc_path" >&2
    exit 1
fi

if [[ ! -f "$patch_path" ]]; then
    echo "missing Multiverse algorithm patch: $patch_path" >&2
    exit 1
fi

if git -C "$mvcc_path" apply --reverse --check "$patch_path" 2>/dev/null; then
    echo "algorithm patch already appears to be applied; start from the broken hooked checkout" >&2
    exit 1
fi

git -C "$mvcc_path" apply --check "$patch_path"

cmake --build "$build_path" --target multiverse_smoke_tests -j2
LINCHECK_MULTIVERSE_BUG_SCHEDULE_FILE="$schedule_file" \
    "$build_path/multiverse_smoke_tests" \
    manual_model_checker_detects_broken_direct_forced_mode2_snapshot_bug

git -C "$mvcc_path" apply "$patch_path"
patch_applied=1

cmake --build "$build_path" --target multiverse_smoke_tests -j2
LINCHECK_MULTIVERSE_BUG_SCHEDULE_FILE="$schedule_file" \
    "$build_path/multiverse_smoke_tests" \
    manual_model_checker_accepts_fixed_direct_forced_mode2_snapshot

echo "Multiverse Mode2 algorithm patch removes the model-checker-discovered bug"
