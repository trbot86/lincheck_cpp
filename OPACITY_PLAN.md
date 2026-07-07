# STM Opacity Checking Status and Plan

Date: 2026-07-06

## Goal

Lincheck++ has an optional STM opacity checker alongside the public ADT linearizability checker. Public linearizability still checks operation return values, exceptions-as-results, post operations, validation callbacks, and real-time order. STM opacity checks the transaction-attempt history produced by explicit `lincheck::stm` value/location hooks.

The current checker is intended for small bounded model-checking and stress histories. A passing opacity check means no opacity violation was found in the retained, hooked, sequentially consistent execution history.

## Implemented

- Generic STM lifecycle hooks for begin, commit, abort, retry, validation, lock events, and attempt metadata.
- Value/location hooks for opacity:
  - `tx_location_init`
  - `tx_location_register`
  - `tx_location_destroy`
  - `tx_read_value`
  - `tx_write_value`
  - `tx_attempt_metadata`
- `StmOpacityHistory` construction from structured `CheckResult::stm_events`.
- Explicit malformed-history failures for missing values, missing initial values, unsupported value payloads, duplicate transaction ends, unknown transaction accesses, and invalid retry references.
- `StmLifetimePolicy::value_history_only` as the default opacity history-building policy. Raw addresses are treated as value-history logical locations; raw destroy/reuse/use-after-destroy anomalies are counted and sampled as ignored lifetime anomalies instead of failing history construction.
- `StmLifetimePolicy::strict_lifetimes` as opt-in lifecycle/reclamation checking. Strict mode preserves malformed-history failures for destroyed-location access, raw destroy/reuse while transactions are live, ambiguous raw/handle reuse, and missing disambiguating handles.
- Location lifetime generations for safe raw-address reuse after `tx_location_destroy(...)` at a quiescent boundary in strict mode.
- Optional explicit location identity for non-quiescent reuse:
  - `lincheck::stm::ObjectLifetimeHandle`
  - `lincheck::stm::LocationHandle`
  - `lincheck::stm::tx_object<T>` / `lincheck::tx_object<T>`
  - `lincheck::stm::tx_field<T>` / `lincheck::tx_field<T>`
  - backend-facing `make_backend_object_lifetime_handle(...)`, `make_backend_location_handle(...)`, and `BackendLocationRegistry`
  - overloads of `tx_location_init`, `tx_location_register`, `tx_location_destroy`, `tx_read`, `tx_write`, `tx_read_value`, and `tx_write_value` that accept a `LocationHandle`.
  Embedded fields can be identified by object lifetime plus field ID, standalone `tx_field<T>` instances use the field object itself as the allocation unit, and backend adapters can supply allocation tokens/generations without using intrusive test wrappers.
- A transaction-level opacity verifier that:
  - serializes committed transaction attempts subject to visible real-time order,
  - validates read-own-write behavior,
  - validates reads against the latest committed serial-state value or the registered initial value,
  - treats aborted and live transactions as observers over serial prefixes,
  - keeps aborted/live writes local to their own attempt,
  - enforces visible real-time order between committed transactions and observers,
  - enforces visible real-time prefix compatibility between observers.
- Conservative read-from and ordering-graph pruning before committed-order backtracking:
  - visible real-time edges and sound read-from edges are transitive-closed,
  - ordering cycles are rejected before exploring committed orders,
  - blocked committed candidates are skipped until required predecessors are placed,
  - ambiguous non-initial reads with multiple possible committed writers are skipped until at least one possible source writer is placed,
  - ambiguous reads are skipped when the current prefix's latest placed writer to that location is a conflicting writer that overwrites every currently available source,
  - ambiguous source choices that ordering constraints force after the reader are eliminated before search,
  - ambiguous source choices that are forced before a conflicting writer that must also precede the reader are eliminated before search,
  - matching initial-value sources are eliminated when a conflicting writer must precede the reader,
  - a remaining single viable non-initial source is promoted to a normal read-from edge.
- Conservative failed-prefix memoization for committed-order search. Prefixes are memoized only when no complete committed serial order is reachable below that prefix, so observer-specific failures do not prune different prefix orders unsafely.
- Explicit committed-order search limit reporting with `StmOpacityStatus::search_limit_exceeded`.
- Runner integration through `ModelCheckingOptions::check_opacity()` and `StressOptions::check_opacity()`.
- Lifetime policy setters:
  - `ModelCheckingOptions::opacity_lifetime_policy(...)`
  - `StressOptions::opacity_lifetime_policy(...)`
  - `StmOpacityHistoryBuildOptions::lifetime_policy`
- Search cap APIs:
  - `ModelCheckingOptions::opacity_max_committed_orders(...)`
  - `StressOptions::opacity_max_committed_orders(...)`
- Structured result fields:
  - `CheckResult::opacity_checked`
  - `CheckResult::opacity_history`
  - `CheckResult::opacity_result`
  - `CheckResult::opacity_explanation`
- Failure kind `FailureKind::opacity_violation`.
- Combined failure traces when public ADT linearizability and STM opacity both fail for the retained run.
- Failure trace `stm opacity:` sections with lifetime policy, ignored lifetime anomaly counters/samples, transaction counts, search-space upper bound, search limit, explored committed orders, memo entries, memo-pruned prefixes, read-from constraints, ambiguous read-from constraints/prunes, source-before-reader prunes, conflicting-writer prunes, eliminated ambiguous sources, shadowed ambiguous sources, eliminated matching initial sources, promoted ambiguous constraints, ordering-graph edges, transitive edges, candidate prunes, cycle status, observer checks, bounded read-from witnesses, ordering-graph samples, rejected prefix samples, locations, raw-address generations, explicit handle/object/field identity metadata, transactions, and access values.
- Backend-neutral export helpers:
  - `format_stm_opacity_history_json(...)`
  - `format_stm_opacity_history_dot(...)`
- Backend-neutral tests for:
  - committed and read-only histories,
  - read-own-write,
  - aborted and live observers,
  - dirty reads,
  - stale reads under real-time order,
  - inconsistent snapshots,
  - incompatible observer prefix ordering,
  - malformed metadata-only histories,
  - unsupported value payloads,
  - duplicate transaction ends,
  - strict safe location destroy/re-register/reinitialize reuse,
  - strict destroyed-location access and unsafe live lifetime reuse,
  - default value-history handling for raw destroyed-location access,
  - default value-history handling for live raw-address reuse where downstream impossible values still fail opacity,
  - embedded fields in CRTP-managed objects,
  - standalone self-owned transactional fields,
  - non-quiescent address reuse with distinct explicit handles,
  - raw-address-only reuse that remains malformed in strict mode when stable handles are required,
  - mixed raw-address and explicit-handle histories for distinct locations,
  - committed-prefix memoization,
  - read-from constraint pruning,
  - ambiguous read-from pruning and fallback,
  - ambiguous source elimination and promotion,
  - ambiguous source shadowing by forced conflicting writers,
  - matching initial-source elimination by forced conflicting writers,
  - generated larger ambiguous read-from spaces,
  - seeded generated ambiguous read-from matrices,
  - generated multi-reader/multi-location ambiguous read-from matrices,
  - generated shadowed-source matrices,
  - generated retry-heavy logical transaction histories,
  - generated larger retry observer matrices,
  - generated long observer chains,
  - generated search-limit, memoization, and mixed possible/impossible histories,
  - generated paired possible/impossible cycle-shaped histories,
  - generated wider observer search-limit histories,
  - ordering-graph cycle rejection,
  - larger observer chains,
  - repeated logical transaction attempts/retries,
  - ambiguous read-from candidates,
  - simultaneous public linearizability and opacity failures,
  - multi-location impossible read-from cycles,
  - opacity failure independent of public ADT linearizability.
- Optional Multiverse adapter coverage for counter, contended/high-contention counter retries, snapshot reads during retry contention, tiny-set, bank, read-only/read-heavy bank snapshots, direct snapshot transaction bank totals with bounded updater detector behavior, same-address field reuse, shared-array smoke scenarios, a passing tiny direct-snapshot shared-array model check, a forced-mode2 constant-total shared-array model-check detector for the known snapshot/updater violation, and a larger constant-total shared-array snapshot/updater stress smoke with retry-safe accumulation.

## Current Opacity Model

For each retained run, Lincheck++ builds a transaction history and asks whether there exists a serial explanation satisfying:

- Committed transactions appear in a serial order that respects visible transaction real-time edges.
- Before backtracking, the verifier derives a conservative committed-transaction ordering graph from visible real-time edges and non-ambiguous read-from constraints. The graph is transitive-closed; cycles are immediate opacity violations, and candidates blocked by unplaced predecessors are skipped during search.
- Under the default `value_history_only` lifetime policy, raw-address-only locations are value-history logical locations. Raw destroy hooks, accesses after raw destroy, raw reuse while transactions are live, and raw/handle overlap are recorded as ignored lifetime anomalies where possible, not used to reject the history. The verifier still checks the resulting value-bearing reads/writes for opacity.
- Under `strict_lifetimes`, raw-address-only reuse is represented as a new location generation only after the old generation is destroyed and no transaction is live. Accessing a destroyed raw generation, destroying a raw-address location while transactions are live, reusing a raw address during a live transaction, or mixing ambiguous raw/handle identity is malformed.
- Explicit handles can represent non-quiescent reuse when every location lifecycle/read/write event carries an unambiguous stable handle. For embedded fields, the intended identity is object lifetime plus field ID. For standalone `tx_field<T>`, the field object itself is the allocation unit. Raw and explicit identities can coexist in one history for distinct locations. Strict same-address live reuse must stay fully handle-based.
- A transaction reads its own earlier write to a location when present.
- Otherwise, a read observes the latest preceding committed write to that location, or the registered initial value.
- Aborted and live transaction attempts are observers that must fit at a serial prefix.
- Observer writes are visible only to later reads in the same observer attempt.
- Observer prefix choices must respect visible real-time order against committed transactions and against other observers.
- Live transactions at the end of the retained history are checked as partial aborted observers.

The read-from pruning is deliberately conservative. It adds an edge when a non-initial read has exactly one possible committed final writer, or when an initial-value read has no matching committed final writer and therefore must precede conflicting writers. For ambiguous reads, the verifier records a disjunctive choice. If ordering constraints already force a possible source after the reader, that source is eliminated. If ordering constraints force a possible source before a conflicting writer that must also precede the reader, that source is eliminated because it cannot be the latest visible value. If the initial value matches but a conflicting writer must precede the reader, the implicit initial source is eliminated. If a non-initial ambiguous read has one viable committed source left, that source is promoted to a normal read-from edge. If the initial value is not a legal source, the reader cannot be placed until at least one possible committed source writer is already in the serial prefix. For all ambiguous choices, including choices where the initial value is legal, the reader is also skipped when the current prefix's latest placed writer to that location is a conflicting writer. Ambiguous cases that survive those prefix checks are left to the normal serial-state search. Diagnostics retain bounded samples for read-from witnesses, ambiguous read-from choices, source elimination/promotion, shadowed sources, initial-source elimination, conflicting-prefix prunes, ordering constraints, and rejected serial prefixes.

This catches impossible committed orders, dirty reads, stale reads, read-from cycles, inconsistent snapshots, observer chains that depend on aborted/live writes, and malformed histories. In default value-history mode it does not directly catch reclamation/reuse-after-free bugs unless they surface as impossible transaction values or public ADT results.

## Lifetime Policy and Non-Quiescent Reuse

The default policy is value-history-first. It is intended for empirically checking a linearizable object built on top of an STM while assuming reclamation/address reuse is either correct or out of scope. Raw-address lifetime anomalies are retained as diagnostics so a user can see that the history glossed over lifecycle details, but those anomalies do not weaken the serial opacity verifier.

Strict lifetime checking is opt-in. In strict mode, raw-address safe reuse is intentionally quiescent: after `tx_location_destroy(address)`, a later register/init of the same address creates a new generation only when no transaction is live. If any transaction is live, destroy/reuse remains malformed because an address alone is not enough to decide which generation an in-flight transactional access meant.

Non-quiescent reuse is supported only through explicit stable identity. The implemented opt-in path is `LocationHandle`, either built directly by a backend, created with the non-intrusive backend helper APIs, or captured by the intrusive `tx_object<T>` / `tx_field<T>` helper layer. Sufficient hook data includes either:

- a backend-provided generation or allocation token on every location register/init/destroy/read/write event, or
- explicit per-access location handles that remain unique across reuse and are not recycled while any retained history might still reference them.

The history builder treats raw addresses as logical value-history locations by default, or `(raw address, generation)` in strict mode. Explicit handles are preserved under both policies. For object-contained fields, the intrusive helper layer constructs the handle from object lifetime plus field ID; backend adapters can do the same non-intrusively from an allocation token plus field ID or field offset/name. Lincheck intentionally avoids reverse lookup from a field address to a containing object. Overlapping lifetimes are strictly modeled only when every access is unambiguously assigned to one explicit handle, and destroy/reuse events remain diagnostic lifecycle records rather than the source of identity. Without that token, non-quiescent reuse is unsafe to infer in strict mode because a read/write event after reuse could refer to the old lifetime, the new lifetime, or a stale/reclaimed object.

## Multiverse Boundary

Multiverse remains an optional adapter/example backend. Core Lincheck++ opacity APIs and tests depend only on generic `lincheck::stm` events.

The local Multiverse patch and `include/lincheck/multiverse_hooks.hpp` bind Multiverse `tx_field<T>` reads/writes and lifecycle points into generic Lincheck STM hooks. The adapter maps each Multiverse `tx_field<T>` to a self-owned handle-based location using the field object pointer as the allocation token, while preserving raw-address fallback behavior for paths that cannot provide stable handle identity. The patch also exposes a narrow spin-loop scheduling hook for Multiverse versioning waits so model-checked schedules can resume a paused writer rather than failing on a synthetic spin behind that writer's lock, and it fixes a background-thread double-deregister teardown bug found while running the adapter smoke tests. Detailed internal Multiverse switch-point labels are present but inert unless an adapter deliberately binds them for an experiment; the default binding does not try to schedule every atomic read/write inside Multiverse.

The smoke suite includes bounded application coverage for normal read transactions, direct `TX_IS_SNAPSHOT` transaction calls where the checkout exposes them, concurrent updater/read mixes, retry-heavy updater scenarios, and detector-style snapshot/updater cases. The shared-array direct-snapshot coverage currently includes a tiny passing model-check case and a larger bounded stress smoke with retry-safe accumulation. The optional forced-mode2 shared-array detector uses a test-only adapter hook to put Multiverse into mode2, preempts a transfer after its decrement-side write and before its increment-side write, and expects Lincheck to report the resulting impossible snapshot total/read-from cycle. That mode forcing is an application-layer detector for a specific Multiverse behavior, not a core Lincheck feature. Multiverse-specific hook call sites, patching, tests, and examples should stay isolated from the core opacity verifier.

## Remaining Work

- Consider additional safe pruning for ambiguous/disjunctive read-from choices beyond source elimination, source shadowing, initial-source elimination, source-before-reader, and conflicting-latest-writer prefix prunes.
- Consider richer backend adapters that can provide stronger owning-object identity than self-owned field handles where the STM exposes allocation-object metadata.
- Add still larger generated histories only if the verifier needs randomized ambiguity, retry, observer, memoization, and search-limit spaces beyond the current deterministic small/medium matrices.
- Add optional Multiverse adapter scenarios beyond bounded smoke/application coverage if a target workload needs higher contention, longer runtimes, or cleaner public hooks for internal mode transitions than the current test-only forced-mode2 helper.
- Consider a fuller setup helper for cloning/checking out/applying the Multiverse patch. A lightweight `tools/check_multiverse_patch.sh` validation helper exists for patched checkouts.

## Non-Goals

- No C++ weak-memory opacity proof.
- No transparent compiler or whole-program instrumentation.
- No proof for unhooked transactional accesses.
- No checking of external side effects performed inside aborted transactions.
- No initial support for open nesting, closed nesting, privatization safety, or default memory reclamation correctness. Reclamation/address-reuse checking is available only through opt-in strict lifetime instrumentation and remains bounded by the emitted hooks.

## Acceptance Baseline

- Core opacity tests pass without Multiverse present.
- Multiverse opacity smoke tests pass when the patched checkout is present.
- Existing behavior with `check_opacity()` disabled remains unchanged.
- Default and Clang CTest suites pass.
- `git diff --check` passes.
- The Multiverse patch remains reversible and matches the patched checkout.
