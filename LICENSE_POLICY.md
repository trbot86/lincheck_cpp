# License and Third-Party Source Policy

This workspace is a private-use C++ port inspired by JVM Lincheck. The implementation should be treated as a behavioral reimplementation unless a future license review explicitly approves copying upstream source text.

## Upstream Sources

- JVM Lincheck is checked out under `upstream/lincheck-jvm` for study and comparison. The repository root is MPL 2.0, and some source files contain older LGPL-style headers.
- Multiverse/MVCC TM is checked out under `upstream/mvcc_tm` as an editable integration target.
- Both upstream trees are ignored by the top-level repository so they remain separate checkouts.

## Copying Policy

- Prefer reimplementing APIs, algorithms, and tests from observed behavior.
- Do not copy nontrivial upstream source code into `include/`, `tests/`, or other top-level tracked files without a separate review of the exact file license and required notices.
- Short names, API concepts, and externally documented behavior may be mirrored when needed for compatibility.

## Multiverse Patch Policy

The local Multiverse hook instrumentation is preserved as `third_party/mvcc_tm-lincheck-hooks.patch`. Apply it to the pinned `mvcc_tm` commit documented in `PORTING_LINCHECK_TO_CPP.md` before building `multiverse_smoke_tests`.

The patch is an integration shim for private testing. It adds no-op hook macros by default and call sites that bind to Lincheck only when `include/lincheck/multiverse_hooks.hpp` is included first.

`third_party/mvcc_tm-mode2-algorithm-fixes.patch` is intentionally separate from the hook instrumentation. It is an optional local algorithm patch used by `tools/check_multiverse_mode2_algorithm_fix.sh` to demonstrate that the model checker finds the broken Mode2 snapshot/updater behavior before the patch and no longer sees that bug after the patch is applied.

## Dependencies

- The C++ port itself is header-only and uses the C++20 standard library.
- Tests use small self-contained executables instead of an external test framework.
- CI fetches Multiverse from GitLab and applies the local hook patch before running the Multiverse smoke tests.
