# Lincheck++

This project is a header-only C++20 Lincheck-style test library. It can run stress tests and bounded cooperative model checks for explicitly instrumented C++ concurrent objects.

This README is the main usage guide. The older status-heavy project inventory is archived in `README_ARCHIVE.md`, and the STM opacity status/design notes live in `OPACITY_PLAN.md`.

The important constraint is that this is not JVM Lincheck-style transparent instrumentation. The checker observes only registered public operations and code paths that call Lincheck wrappers, source macros, STM hooks, or explicit switch points.

## Build

From this repository:

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

To use the library from another CMake target, compile as C++20 and add this repository's `include/` directory:

```cmake
target_compile_features(my_test PRIVATE cxx_std_20)
target_include_directories(my_test PRIVATE /path/to/lincheck/include)
```

Then include either the monolithic header or component forwarding headers:

```cpp
#include <lincheck/lincheck.hpp>
// or:
#include <lincheck/model_checking.hpp>
#include <lincheck/wrappers/atomic.hpp>
```

## Basic ADT Check

Define a concurrent object and a sequential model with matching public operations:

```cpp
#include <lincheck/lincheck.hpp>

#include <iostream>

struct ConcurrentCounter {
    lincheck::atomic<int> value{0};

    int inc() {
        return value.fetch_add(1) + 1;
    }
};

struct SequentialCounter {
    int value = 0;

    int inc() {
        return ++value;
    }
};

int main() {
    auto spec = lincheck::test<ConcurrentCounter, SequentialCounter>()
        .operation("inc", &ConcurrentCounter::inc, &SequentialCounter::inc);

    auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .max_schedule_length(8)
        .check("counter", spec);

    if (!result.success) {
        std::cerr << result.trace << "\n";
        return 1;
    }
}
```

The sequential model is copied during verifier search. If it is not copyable, provide `sequential_cloner(...)`. If the state is large, add `sequential_state(...)` to enable verifier memoization.

## Stress vs Model Checking

Use stress mode for repeated real-thread executions:

```cpp
auto result = lincheck::StressOptions()
    .iterations(10)
    .invocations_per_iteration(100)
    .threads(4)
    .actors_per_thread(2)
    .invocation_timeout(std::chrono::milliseconds(100))
    .check("counter-stress", spec);
```

Use model checking for reproducible cooperative schedules:

```cpp
auto result = lincheck::ModelCheckingOptions()
    .threads(2)
    .actors_per_thread(1)
    .max_schedule_length(16)
    .max_context_switches_per_schedule(3)
    .check("counter-model", spec);
```

Model-checker failures expose `CheckResult::schedule`, `CheckResult::schedule_decisions`, and a textual trace. Reproduce a failure with:

```cpp
auto replayed = lincheck::ModelCheckingOptions()
    .replay(spec, result.scenario, result.schedule);
```

Use `replay(...)` when the schedule must be consumed exactly. Use `check_schedule_prefix(...)` when a discovered prefix should be driven first, but corrected code may legitimately need extra scheduler choices to drain, retry, or finish:

```cpp
auto checked = lincheck::ModelCheckingOptions()
    .check_schedule_prefix(spec, result.scenario, result.schedule);
```

## Model-Checking Coverage

Model checking is bounded exhaustive over the cooperative scheduling choices it observes. It is not an exhaustive permutation of all native thread interleavings or all instruction-level events.

The checker can branch only at Lincheck-visible switch points:

- wrapper operations such as `lincheck::atomic`, `lincheck::mutex`, `lincheck::thread`, waits, semaphores, latches, and barriers
- source macros such as `LC_READ`, `LC_WRITE`, `LC_CALL`, and `LC_SWITCH`
- STM hooks such as Multiverse transaction begin/read/write/validate/commit/abort/retry events
- explicit `lincheck::switch_point(...)` or `LC_SWITCH()`

Exploration is bounded by these options:

- `max_schedule_length(...)`: maximum replay schedule prefix length retained and explored
- `max_context_switches_per_schedule(...)`: maximum thread changes in a schedule, or `-1` for the schedule-length-derived limit
- `max_switch_points_per_schedule(...)`: cooperative switch-point budget for a single run
- `invocations_per_iteration(...)`: maximum schedules explored for each generated or hand-authored scenario
- optional reductions such as `operation_context_reduction()` and `event_dependency_reduction()`

A passing model check therefore means no violation was found among the observed schedules inside those bounds. It does not say anything about uninstrumented races, raw blocking APIs, weak-memory reorderings, or switch points beyond the configured limits.

Use `CheckResult::stats` to see how much of that bounded space was actually explored. Model-checker failure traces and replay failure traces append a `model-checking stats:` section containing `schedules_explored`, `schedules_generated`, pruning counters, `max_context_switch_depth_explored`, `retained_schedule_length`, `retained_schedule_context_switches`, and `retained_schedule_decisions`. Call `format_model_checking_stats(result)` when you want to write the same section to debug or info logs on successful checks. The retained decision count is the number of Lincheck-visible scheduling decisions in the retained failure run, not the number of native scheduler preemptions. If failure minimization is enabled, those retained-run counters describe the minimized reproducer.

Increase the bounds when you want broader schedule exploration, but expect runtime to grow quickly. For STM-backed objects, higher contention can also increase abort/retry rates enough to dominate the test.

## Required Instrumentation

There is no automatic compiler pass that inserts switch points around arbitrary C++ memory accesses. You must make shared behavior visible manually.

Use Lincheck wrappers for shared state and synchronization:

```cpp
lincheck::atomic<int> counter{0};
lincheck::var<int> plain_value{0};
lincheck::mutex lock;
lincheck::condition_variable cv;
lincheck::thread worker([&] { counter.fetch_add(1); });
worker.join();
```

Available wrappers include:

- `lincheck::atomic<T>` and `lincheck::atomic_ref<T>`
- `lincheck::var<T>`
- `lincheck::mutex`, `recursive_mutex`, `timed_mutex`, shared mutexes, and RAII lock wrappers
- `lincheck::condition_variable`
- `lincheck::parker`
- `lincheck::counting_semaphore`, `binary_semaphore`, `latch`, and `barrier`
- `lincheck::thread`, `jthread`, and `lincheck::this_thread` helpers

For plain fields that you do not want to convert to wrapper types, use source macros:

```cpp
struct Counter {
    int value = 0;

    int inc() {
        int observed = LC_READ(value);
        LC_WRITE(value, observed + 1);
        LC_SWITCH();
        return observed + 1;
    }
};
```

Raw shared C++ variables that are raced without wrappers or macros are still ordinary C++ data races and are undefined behavior. The checker cannot make those safe or observable after the compiler optimizes them.

## Hand-Authored Scenarios

For a precise regression, build an explicit scenario:

```cpp
lincheck::ExecutionScenario scenario;
scenario.parallel = {
    {lincheck::Actor{.operation_index = 0, .name = "inc"}},
    {lincheck::Actor{.operation_index = 0, .name = "inc"}}
};

auto result = lincheck::ModelCheckingOptions()
    .max_schedule_length(8)
    .check("manual-counter", spec, scenario);
```

The operation index must match the registration order in the spec. Arguments go in `Actor::arguments` as `lincheck::Value`.

## Operation Options

Operations can be marked for generation and reduction behavior:

```cpp
auto spec = lincheck::test<Table, SeqTable>()
    .operation_with_options(
        "resize",
        &Table::resize,
        &SeqTable::resize,
        lincheck::non_parallel_group("shape")
    )
    .operation_with_options(
        "initialize_once",
        &Table::initialize_once,
        &SeqTable::initialize_once,
        lincheck::one_shot()
    );
```

Use `exceptions_as_results()` when an operation's documented behavior includes throwing and the sequential model throws the same logical result.

## Validation and State

Use `validation(...)` for invariants that are not directly exposed through public operation returns:

```cpp
auto spec = lincheck::test<ConcurrentCounter, SequentialCounter>()
    .operation("inc", &ConcurrentCounter::inc, &SequentialCounter::inc)
    .state_representation([](const ConcurrentCounter& counter) {
        return "value=" + std::to_string(counter.value.load());
    })
    .validation([](const ConcurrentCounter& counter) {
        return counter.value.load() >= 0 ? std::string{} : "negative counter";
    });
```

If a bug should be reported as `invalid_results`, expose the relevant state through a registered public operation and include it in the scenario, often as a post operation. Validation failures are reported separately as `validation_failure`.

## Source Audit and Rewrite Tools

Use `lincheck_source_audit` to find raw synchronization APIs that still need wrappers or adapters:

```bash
cmake --build build --target lincheck_source_audit
./build/lincheck_source_audit --exclude=build --exclude=upstream path/to/source-or-tree
```

It is lexical and conservative. It honors:

- `--include=TEXT`
- `--exclude=TEXT`
- `--allow-token=TEXT`
- `--allow-line=TEXT`
- `// NOLINT(lincheck-raw-sync)`
- `// NOLINTNEXTLINE(lincheck-raw-sync)`

Use `lincheck_source_rewrite` only as a convenience bridge for small files:

```bash
cmake --build build --target lincheck_source_rewrite
./build/lincheck_source_rewrite path/to/source.cpp > path/to/source.lincheck.cpp
./build/lincheck_source_rewrite --check path/to/source-tree
./build/lincheck_source_rewrite --in-place path/to/source-tree
```

The rewriter is lexical. It is not a parser, not a compiler pass, and not whole-program instrumentation. Review generated changes before trusting them.

When Clang/LLVM development packages are installed, CMake can also build the AST audit prototype. It is off by default because ordinary Clang compiler packages often do not include complete LibTooling CMake targets:

```bash
cmake -S . -B build-clang-tools -DLINCHECK_BUILD_CLANG_TOOLS=ON
cmake --build build-clang-tools --target lincheck_clang_source_audit
./build-clang-tools/lincheck_clang_source_audit path/to/source.cpp -- -std=c++20 -I/path/to/lincheck/include
```

This tool checks main-file declarations and direct calls for raw standard synchronization APIs. It is also only an audit tool. It does not rewrite or instrument the program.

## Transactional Memory

Lincheck++ supports STM-backed objects through explicit STM hooks. The hooks do not replace the normal ADT specification: you still register public operations against a sequential model, and Lincheck verifies the public operation history for linearizability.

The STM extension gives the model checker visibility inside a transaction implementation. A backend can call:

- `lincheck::stm::tx_location_init(address, value)`
- `lincheck::stm::tx_location_init(location_handle, value)`
- `lincheck::stm::tx_location_register(address, label, type_name)`
- `lincheck::stm::tx_location_register(location_handle, label, type_name)`
- `lincheck::stm::tx_location_destroy(address)`
- `lincheck::stm::tx_location_destroy(location_handle)`
- `lincheck::stm::tx_begin(read_only, start_clock_or_version)`
- `lincheck::stm::tx_read(address, lock_slot, version)`
- `lincheck::stm::tx_read(location_handle, lock_slot, version)`
- `lincheck::stm::tx_write(address, lock_slot)`
- `lincheck::stm::tx_write(location_handle, lock_slot)`
- `lincheck::stm::tx_read_value(address, value, lock_slot, version)`
- `lincheck::stm::tx_read_value(location_handle, value, lock_slot, version)`
- `lincheck::stm::tx_write_value(address, value, lock_slot)`
- `lincheck::stm::tx_write_value(location_handle, value, lock_slot)`
- `lincheck::stm::tx_validate_begin()` and `lincheck::stm::tx_validate_end(success)`
- `lincheck::stm::tx_lock_attempt(lock_slot)`, `lincheck::stm::tx_lock_acquired(lock_slot)`, `lincheck::stm::tx_lock_failed(lock_slot)`, and `lincheck::stm::tx_lock_released(lock_slot)`
- `lincheck::stm::tx_commit_attempt()` and `lincheck::stm::tx_commit_success(commit_clock)`
- `lincheck::stm::tx_abort(reason)` and `lincheck::stm::tx_retry(reason, attempt)`
- `lincheck::stm::tx_attempt_metadata(logical_transaction_id, attempt)`

When a Lincheck runtime is active, each hook records a structured `CheckResult::stm_events` entry, emits a trace event with transaction ID/depth metadata, and creates a cooperative scheduler switch point for model checking. Hooks are cheap outside Lincheck runs, but they are explicit instrumentation; there is no compiler pass that inserts them automatically.

Raw-address hooks are enough for the default value-history opacity mode. For tests that opt into strict lifetime/reclamation checking, or for adapters that want diagnostics to retain object identity across reuse, use explicit handles. The optional intrusive helper layer gives tests and adapters a compact way to do that without reverse lookup from a field address to its containing object:

```cpp
struct Node : lincheck::tx_object<Node> {
    lincheck::tx_field<int> value;
    lincheck::tx_field<int> next_index;

    Node(int value_initial, int next_initial)
        : value(lc_object_handle(), "value", value_initial),
          next_index(lc_object_handle(), "next_index", next_initial) {}
};
```

`tx_object<T>` creates an object lifetime handle during base construction. Each embedded `tx_field<T>` stores a location handle made from that object lifetime plus the supplied field ID, then emits handle-based register/init/read/write/destroy hooks when a Lincheck runtime is active. A standalone `lincheck::tx_field<T> counter{0};` is self-owned: the field object itself is treated as the allocation unit with field ID `self`.

Backends that already have allocation identity do not need to use `tx_object<T>`. They can construct `lincheck::stm::ObjectLifetimeHandle` and `lincheck::stm::LocationHandle` values directly, or use the non-intrusive helpers:

```cpp
lincheck::stm::BackendLocationRegistry registry("my-stm");
auto location = registry.get_or_create_location(
    allocation_token,
    field_address,
    "value",
    "Node.value",
    "int"
);

lincheck::stm::tx_location_register(location);
lincheck::stm::tx_location_init(location, 0);
lincheck::stm::tx_read_value(location, observed_value, lock_slot, version);

if (auto destroyed = registry.destroy_location(allocation_token, field_address)) {
    lincheck::stm::tx_location_destroy(*destroyed);
}
```

Use `make_backend_object_lifetime_handle(...)` or `make_backend_location_handle(...)` when the backend can supply its own allocation ID, allocation token, optional raw object address, optional generation, field ID or field name/offset, raw field address, and diagnostic label/type name. `BackendLocationRegistry` is the convenience form for adapters that see construction/destruction and want Lincheck to assign generations per backend token. These helpers are backend-neutral and do not depend on Multiverse. Raw-address hooks remain valid for ordinary opacity checking; strict lifetime mode needs handles or quiescent raw-address generations to disambiguate same-address live reuse.

Use this pattern for an STM-backed ADT:

1. Implement the concurrent object with the STM's normal transaction API.
2. Register only the public ADT operations in `lincheck::test<Concurrent, Sequential>()`.
3. Provide a sequential model for those public operations.
4. Add `state_representation(...)`, `validation(...)`, or post operations when important state is not otherwise observable through returns.
5. Install STM hook calls in the backend or include a backend-specific Lincheck binding header before the STM header.
6. Run both bounded model checks for small representative scenarios and stress checks for real-thread retry behavior.

By default, STM hooks are scheduling and diagnosis aids for public ADT linearizability. A passing model check means no public linearizability violation was found among the visible, bounded hook schedules.

Enable the separate STM opacity checker when the backend emits value/location hooks:

```cpp
auto model_result = lincheck::ModelCheckingOptions()
    .check_opacity()
    .max_schedule_length(16)
    .check("stm-model", spec);

auto stress_result = lincheck::StressOptions()
    .check_opacity()
    .iterations(10)
    .check("stm-stress", spec);
```

`check_opacity()` uses `StmLifetimePolicy::value_history_only` by default. This is the normal mode for checking whether an STM-backed ADT produces opaque transaction values and linearizable public results while treating reclamation/address reuse as out of scope. Raw addresses are treated as logical value-history locations. Raw-address destroy hooks, use-after-destroy, and same-address reuse while transactions are live are recorded as ignored lifetime anomalies instead of malformed histories. If reuse produces a bad value later, opacity checking or public ADT linearizability can still reject the retained execution.

Opt into strict lifetime/reclamation checking only when the backend emits enough identity to make that meaningful. Strict mode is for tests that deliberately want address-reuse, generation, and reuse-after-free mistakes to fail at history-build time:

```cpp
auto strict_result = lincheck::ModelCheckingOptions()
    .check_opacity()
    .opacity_lifetime_policy(lincheck::StmLifetimePolicy::strict_lifetimes)
    .check("stm-strict-lifetimes", spec);
```

Direct history-building helpers take the same policy:

```cpp
auto history = lincheck::build_stm_opacity_history(
    events,
    lincheck::StmOpacityHistoryBuildOptions{
        .lifetime_policy = lincheck::StmLifetimePolicy::strict_lifetimes
    }
);
```

Opacity mode builds a transaction history from `CheckResult::stm_events`. Committed transactions must fit into a serial order; aborted and live transaction attempts must be placeable as observers of a consistent serial prefix while respecting visible real-time order between transactions. A read may observe a prior write in the same transaction, the latest preceding committed write, or the registered initial value for the location. Before committed-order backtracking, the verifier derives conservative ordering edges from visible real-time order and sound read-from constraints, rejects cycles, skips candidates whose required predecessors are not yet placed, eliminates ambiguous sources forced after the reader, eliminates ambiguous sources shadowed by forced conflicting writers, eliminates matching initial sources shadowed by forced conflicting writers, promotes a non-initial ambiguous read with one viable source left to a normal read-from edge, and prunes ambiguous read-from candidates whose current prefix cannot still source the observed value.

Manual requirements for opacity mode:

- Emit `tx_location_init(address, value)` before a checked location is first read or written.
- Use `tx_read_value(...)` and `tx_write_value(...)` for checked accesses; metadata-only `tx_read(...)` and `tx_write(...)` are not enough and fail as malformed opacity histories.
- Make values convertible to `lincheck::Value`; unsupported value payloads fail explicitly.
- In default value-history mode, inspect `opacity_history.ignored_lifetime_anomalies`, `opacity_result.ignored_lifetime_anomalies`, and their sample vectors when raw destroy/reuse events were present.
- In strict lifetime mode, safe raw-address reuse after `tx_location_destroy(address)` is modeled as a new location generation only across a quiescent boundary with no live transaction. Destroyed raw-location access, raw destroy while transactions are live, raw reuse during a live transaction, and ambiguous raw/handle reuse are malformed histories.
- Strict non-quiescent reuse needs backend-provided lifetime identity on every relevant lifecycle/read/write event, such as object lifetime plus field ID or a stable per-access `LocationHandle`. Raw address identity alone is not enough to model reuse while transactions are live.
- Emit transaction begin/end hooks around every attempt. Attempts with begin and no commit/abort are treated as live observers.
- Emit `tx_attempt_metadata(logical_transaction_id, attempt)` when the STM distinguishes logical transactions from retry attempts; this is mostly diagnostic but makes failures readable.

Committed transaction ordering is searched with conservative read-from pruning, ordering-graph pruning, ambiguous read-from prefix pruning, and failed-prefix memoization by default for the retained history. The read-from pruning adds non-disjunctive edges for unique non-initial committed writers and initial-value reads with no matching committed writer. Ambiguous reads record a disjunctive source choice: sources that must occur after the reader are eliminated, sources shadowed by forced conflicting writers are eliminated, matching initial sources shadowed by forced conflicting writers are eliminated, a non-initial read with one viable source left is promoted to a normal edge, non-initial ambiguous reads require at least one possible committed source before the reader, and all ambiguous reads are skipped when the prefix's latest placed writer to the location is a conflicting writer. Ambiguous cases that survive those checks are left to serial-state search. To cap verifier work, configure a committed-order search limit:

```cpp
auto result = lincheck::ModelCheckingOptions()
    .check_opacity()
    .opacity_max_committed_orders(10000)
    .check("stm-model", spec);
```

If the cap is reached before an opaque committed order is found, `opacity_result.status` is `StmOpacityStatus::search_limit_exceeded` and the check fails with `FailureKind::opacity_violation`. A zero limit is the default and means unbounded search for the retained history. The built-in core tests use deterministic small/medium generated histories for ambiguity, retries, observer chains, memoization/search-limit behavior, and paired possible/impossible shapes; use the same style for backend-neutral verifier coverage instead of relying only on one hand-written counterexample.

Opacity failures use `FailureKind::opacity_violation`. The result exposes `opacity_checked`, `opacity_history`, `opacity_result`, and `opacity_explanation`; failure traces include an `stm opacity:` section with the status, explanation, lifetime policy, ignored lifetime anomaly count/samples, committed transaction count, observer transaction count, committed-order search-space upper bound, search limit, explored committed-order count, memo entries, memo-pruned prefixes, read-from constraint count, ambiguous read-from constraints/prunes, source-before-reader prunes, conflicting-writer prunes, eliminated ambiguous sources, shadowed ambiguous sources, eliminated matching initial sources, promoted ambiguous constraints, ordering-graph edge/transitive-edge/candidate-prune counts, ordering-cycle status, observer checks, bounded read-from witnesses, ordering-graph samples, rejected prefix samples, locations with raw address/generation metadata and explicit handle/object/field metadata, transactions, and access values. If public ADT verification and opacity both fail for the retained run, the trace includes both the verifier explanation and the opacity section.

Use `format_stm_opacity_history_json(result.opacity_history, &result.opacity_result)` or `format_stm_opacity_history_dot(result.opacity_history, &result.opacity_result)` to export the retained opacity history for offline debugging. JSON and DOT exports include lifetime policy, ignored lifetime anomaly counters/samples, raw address/generation metadata, explicit handle/object/field metadata, and bounded diagnostic samples from the result object where practical. These helpers are backend-neutral; they operate only on the generic Lincheck STM history.

This is still bounded checking over visible hooks. It does not prove opacity for unhooked STM paths, external side effects inside aborted transactions, privatization safety, reclamation safety, open/closed nesting, or weak-memory behavior. The checker assumes sequential consistency. Reclamation/address-reuse bugs are out of scope in the default value-history policy and require opt-in strict lifetime instrumentation.

High-contention STM workloads can abort and retry heavily. Dense key or slot overlap, high transaction sizes, high thread counts, or large iteration counts can make tests look like deadlock or livelock because transactions keep retrying. Start with small domains and low expected overlap, then raise contention deliberately with explicit invocation timeouts, retry/attempt expectations in the test object when practical, and an opacity committed-order limit.

## Multiverse STM Tests

The Multiverse integration is an optional adapter/example backend. Core Lincheck++ opacity APIs, history building, verification, diagnostics, and generic tests do not depend on a Multiverse checkout. The Multiverse smoke tests have manual setup because the upstream checkout must be patched with hook call sites.

For a fresh checkout:

```bash
mkdir -p upstream
git clone --recursive https://gitlab.com/Coccimiglio/mvcc_tm.git upstream/mvcc_tm
git -C upstream/mvcc_tm checkout 0d1454cb7d902d6534cb79dc788f08395906bb44
git -C upstream/mvcc_tm apply --unidiff-zero ../../third_party/mvcc_tm-lincheck-hooks.patch
```

Then configure and run:

```bash
cmake -S . -B build
cmake --build build --target multiverse_smoke_tests -j2
./build/multiverse_smoke_tests
```

For an already patched checkout, validate patch reversibility and equality with:

```bash
bash tools/check_multiverse_patch.sh
```

Pass alternate paths as `bash tools/check_multiverse_patch.sh <mvcc_dir> <patch_file>`.

The repository also keeps an optional Multiverse Mode2 algorithm patch separate from the hook patch:

```bash
third_party/mvcc_tm-mode2-algorithm-fixes.patch
```

The normal local checkout is expected to be hook-patched but still algorithmically broken, so Lincheck can demonstrate the bug. To run the full before/after routine:

```bash
bash tools/check_multiverse_mode2_algorithm_fix.sh
```

That script builds the broken hooked checkout, runs the model checker until it finds the direct-forced Mode2 snapshot/updater counterexample, saves the discovered schedule, applies `mvcc_tm-mode2-algorithm-fixes.patch`, rebuilds, checks the discovered schedule prefix against the fixed checkout, and reverses the algorithm patch before exiting. The manual detector tests are skipped by default in `./build/multiverse_smoke_tests`; run them through the script unless you are deliberately debugging the patch.

To bind hooks in code, include the Lincheck binding before Multiverse:

```cpp
#include <lincheck/multiverse_hooks.hpp>
#include "multiverse/multiverse.hpp"
```

The hook macros are no-ops unless the binding header is included first. The local patch emits value-bearing `tx_field` read/write hooks, lazy location initialization for fields first observed under a Lincheck runtime, location destroy events, logical transaction attempt metadata, and a narrow spin-loop scheduling hook for Multiverse versioning waits. It also fixes a Multiverse background-thread double-deregister teardown bug that ASan exposed while running the smoke tests. The spin-loop hook exists so the model checker can resume a paused writer instead of reporting a synthetic spin behind that writer's lock; it is not a transparent scheduler for every atomic read/write inside Multiverse. Detailed internal Multiverse switch-point labels remain inert unless an adapter deliberately binds them for a local experiment.

The binding maps each Multiverse `tx_field<T>` to a self-owned handle-based location using the field object pointer as the allocation token and the contained value address as the raw field address. Opacity histories therefore retain object-lifetime IDs, generations, field ID `self`, and raw addresses for Multiverse fields where practical. Paths that cannot expose a stable `tx_field<T>` token continue to use the raw-address fallback; under the default value-history policy this is still usable for ordinary opacity checking, while strict lifetime mode needs stable handles or quiescent raw lifetimes. With `check_opacity()`, Lincheck checks the retained Multiverse transaction history for the bounded opacity property above. Without `check_opacity()`, it verifies only public ADT linearizability. The current adapter smoke tests cover normal read transactions, direct `TX_IS_SNAPSHOT` calls where Multiverse exposes that transaction type, bounded concurrent updater/read mixes, retry-heavy updater scenarios, snapshot/updater cases, and a manual direct-forced Mode2 detector for the known snapshot/updater bug. The default Mode2 smoke enters Mode2 through Multiverse's transition protocol in scenario setup instead of directly switching modes in the middle of an updater; the manual detector intentionally uses a test-only direct mode switch to reproduce the bug under the model checker. Keep this kind of mode control in adapter/application tests, not in core Lincheck behavior. Keep Multiverse-specific bindings in adapter/test code; the core verifier should continue to depend only on generic `lincheck::stm` events.

The `multiverse_smoke_tests` CMake target defines `DISABLE_UNVERSIONING=1`. This is deliberate: the smoke suite is checking transaction value histories and public ADT results, not Multiverse's background version reclamation. The rest of the Multiverse examples are left with their normal compile definitions.

High-contention STM workloads can abort and retry heavily. Dense shared-array slot overlap, high `k`, high thread counts, or large iteration counts can look like deadlock or livelock because the STM keeps retrying. Even apparently disjoint array slots can collide in a word-based TM's lock table. Keep smoke-test domains small, estimate expected overlap before increasing parameters, and pair contention-oriented adapter tests with explicit runtime and opacity-search bounds.

The shared-array snapshot/updater smoke uses a constant-total invariant: updater transactions transfer value between slots while a snapshot reader sums the whole array. The direct `TX_IS_SNAPSHOT` model-check variant verifies the hook/opacity path with a 12-slot passing scenario and zero context switches. The protocol Mode2 model-check variant uses a smaller setup: enter Mode2 in scenario init, run a 4-slot transfer/snapshot pair with zero context switches, and release Mode2 in post. The manual direct-forced Mode2 detector uses an 8-slot array and a model-checked updater/snapshot pair; on the broken checkout Lincheck reports a combined invalid-result/opacity violation, then `tools/check_multiverse_mode2_algorithm_fix.sh` applies the optional patch and verifies the discovered schedule prefix no longer produces the bug. The larger Mode2 and direct-snapshot stress variants use retry-safe snapshot accumulators and are expected to pass; a 100k-slot version is too large for the default suite and should be treated as an opt-in benchmark shape.

## Important Limits

- No transparent compiler, LLVM, or Clang instrumentation is implemented.
- No whole-program load/store instrumentation is automated.
- No fibers or coroutines are supported.
- No JVM compatibility layer is provided.
- The checker explores wrapper behavior as sequentially consistent. `memory_model(...)` rejects weak-memory modes today.
- Non-`seq_cst` atomic and fence orders are recorded and warned about, but not weak-memory explored.
- STM opacity checking is opt-in and requires value/location hooks. Metadata-only STM traces are useful for scheduling and diagnostics but are malformed for opacity mode.
- Default STM opacity checking is value-history-only; reclamation/address-reuse bugs require opt-in strict lifetime policy plus sufficient lifetime hooks.
- Model-checker aborts, stress timeouts, and obstruction-freedom checks are cooperative. Code must reach Lincheck callbacks or switch points.
- Raw platform waits, custom spin locks, third-party synchronization primitives, and raw standard waits outside Lincheck wrappers need wrappers, source macros, or explicit adapters.
- `rdtsc`/`rdtscp` clocks can be inconsistent across sockets, NUMA nodes, virtual machines, older CPUs without invariant synchronized TSC, and aggressive power-management settings. Use the atomic-sequence clock when strict cross-thread ordering matters.
