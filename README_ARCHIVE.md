# C++ Lincheck Archive Notes

This is the archived pre-usage README/status inventory. The current usage guide is `README.md`, and STM opacity status/design notes are in `OPACITY_PLAN.md`.

This is a private-use C++ MVP inspired by JVM Lincheck. The upstream JVM source is checked out in `upstream/lincheck-jvm`; the C++ port plan is in `PORTING_LINCHECK_TO_CPP.md`.

The MVP is intentionally explicit rather than transparent: code under test must use the Lincheck wrappers or registered operations for the model checker to observe switch points.

The editable Multiverse/MVCC TM checkout is in `upstream/mvcc_tm`.

## Build and Test

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

`multiverse_smoke_tests` is built when `upstream/mvcc_tm/multiverse/multiverse.hpp` is present. For a fresh checkout, recreate the editable Multiverse target with:

```bash
mkdir -p upstream
git clone --recursive https://gitlab.com/Coccimiglio/mvcc_tm.git upstream/mvcc_tm
git -C upstream/mvcc_tm checkout 0d1454cb7d902d6534cb79dc788f08395906bb44
git -C upstream/mvcc_tm apply --unidiff-zero ../../third_party/mvcc_tm-lincheck-hooks.patch
```

CMake skips the Multiverse smoke tests when that checkout is missing; CI fetches and patches it before running the full suite.

When the patched Multiverse checkout is present, CMake also builds standalone STM-backed examples:

```bash
cmake --build build --target multiverse_tiny_set_example
./build/multiverse_tiny_set_example
cmake --build build --target multiverse_bank_example
./build/multiverse_bank_example
```

## What Is Implemented

- Header-only library at `include/lincheck/lincheck.hpp`, with modular forwarding headers for value, scenario, options, generators, verifier, stress, model checking, runtime, trace, clock, source audit, STM, and wrappers.
- GitHub Actions CI for GCC and Clang CMake builds, including Multiverse smoke tests with the local hook patch applied.
- Private-use license and third-party source policy in `LICENSE_POLICY.md`.
- Explicit data-structure test registration with `lincheck::test<Concurrent, Sequential>()`.
- Operation registration through member-function pointers or typed concurrent/sequential callables.
- Custom concurrent/sequential object factories and custom sequential model cloners for non-default-constructible test types.
- Seeded random scenario generation.
- Parameter generators are reset for each generated scenario.
- Parameter generators for `gen<T>()` defaults, constants, ranges, booleans, enum underlying domains, fixed value domains, string domains, random strings, and custom seeded callables.
- Operation metadata through `operation_with_options`, including operation groups, non-parallel groups, one-shot operations, and opt-in exception-as-result handling.
- Linearizability verification against a copyable sequential model.
- `lincheck::Value` support for `std::optional<T>` and custom user result types with equality, formatting, stable hashing, and `value_cast` recovery.
- Optional verifier memoization of failed and successful search states via `sequential_state(...)` keys for copyable sequential models.
- Post-scenario validation callbacks through `validation(...)`.
- Operation invocation/response clocks with verifier real-time ordering.
- Operation group, non-parallel, and one-shot metadata in scenario and operation trace labels.
- `ClockSource` abstraction with process-global atomic-sequence and `rdtscp`-based implementations.
- Cooperative obstruction-freedom check with a switch-point budget.
- Stress runner using real `std::thread` workers with a ready-count start gate for parallel actors.
- Cooperative stress-runner invocation timeouts at Lincheck switch points.
- Cooperative bounded model checker using real threads and explicit switch points.
- Observed-frontier model-checker traversal that records runnable choices at explicit scheduler decisions and enqueues unexplored alternative prefixes.
- Deterministic model-checker frontier ordering; `seed(0)` preserves FIFO/lexicographic order, while nonzero seeds weight same-depth frontier prefixes and perturb runnable-choice order reproducibly.
- Duplicate successful public-history verifier pruning for completed schedules with the same operations, return values, and real-time-order constraints.
- Opt-in operation-context schedule reduction for operations explicitly marked with the same independence group.
- Opt-in observed event-dependency schedule reduction for runnable operations whose retained public-operation footprints touch disjoint shared resources.
- Optional model-checker context-switch bounds through `max_context_switches_per_schedule(...)`.
- Model-checker switch-point budgets for cooperative livelock detection.
- Cooperative model-checker abort requests that wake and stop peer workers after a primary failure.
- General-purpose real-thread `lincheck::run_concurrent_test(...)` helper for wrapper-instrumented concurrent blocks.
- Source-level instrumentation macros: `LC_READ`, `LC_WRITE`, `LC_CALL`, and `LC_SWITCH`.
- Source-location metadata plus stable object and location IDs in macro-generated trace events.
- Structured source-access records from `LC_READ` and `LC_WRITE`, retained in `CheckResult::source_accesses`.
- Source instrumentation audit API plus `lincheck_source_audit` CLI for flagging raw `std::atomic`, `std::atomic_ref`, `std::thread`, `std::jthread`, standard mutex and shared-mutex wrappers, condition-variable, semaphore, barrier, latch, futex, pthread, and compiler atomic APIs that need Lincheck wrappers or adapters.
- Optional LibTooling-backed `lincheck_clang_source_audit` prototype for AST-based raw standard synchronization checks when Clang/LLVM development packages are available.
- Preliminary lexical source rewriter API plus `lincheck_source_rewrite` CLI for converting common raw standard wrappers, including `std::atomic_ref`, `std::recursive_mutex`, `std::timed_mutex`, `std::recursive_timed_mutex`, `std::shared_mutex`, `std::shared_timed_mutex`, `std::shared_lock`, standard semaphores, standard barriers, and standard latches, to Lincheck wrappers in source files or source directories.
- Traced fence helpers: `lincheck::atomic_thread_fence`, `lincheck::atomic_signal_fence`, `LC_THREAD_FENCE`, and `LC_SIGNAL_FENCE`.
- Structured generic trace events in `CheckResult::trace_events`, including operation start/finish, switch points, thread lifecycle events, obstruction-freedom failure points, and wrapper trace messages retained with unified event indexes.
- Structured `MemoryEvent` callbacks from explicit atomic, atomic-ref, and fence wrappers, carrying operation kind, object identity, memory-order labels, operands, observed values, and CAS success metadata for future memory-model analysis. Model checks, stress checks, and `run_concurrent_test` preserve captured events in `CheckResult::memory_events`.
- Structured synchronization events from mutex, shared mutex, condition-variable, atomic wait/notify, parker, semaphore, latch, and barrier wrappers, retained in `CheckResult::synchronization_events`.
- Event dependency graph summaries in `CheckResult::event_dependencies`, derived from retained trace, memory, STM, source-access, and synchronization streams with per-stream and cross-stream thread/resource edges, optional public-operation labels, per-operation resource footprints in `CheckResult::operation_dependency_footprints`, plus acyclicity diagnostics in `CheckResult::event_dependency_analysis` and JSON/DOT export helpers.
- Managed model-checker mutex ownership, shared ownership, wait queues, recursive depth, timed-lock wake/timeout traces, and deadlock detection for `lincheck::mutex`, `lincheck::recursive_mutex`, `lincheck::timed_mutex`, `lincheck::recursive_timed_mutex`, `lincheck::shared_mutex`, and `lincheck::shared_timed_mutex`.
- `lincheck::lock_guard`, `lincheck::scoped_lock`, `lincheck::unique_lock`, `lincheck::shared_lock`, and `lincheck::condition_variable`, including managed wait/notify and timed-wait traces.
- `lincheck::recursive_mutex` with real-thread reentrant locking and managed scheduler recursive-depth tracking.
- `lincheck::timed_mutex`, `lincheck::recursive_timed_mutex`, and `lincheck::shared_timed_mutex` with real-thread timed locking plus managed scheduler timed wait queues and timeout traces.
- Templated `lincheck::lock_guard` support for RAII locking and `adopt_lock`; variadic `lincheck::scoped_lock` support for multi-mutex RAII locking and `adopt_lock`; templated `lincheck::unique_lock` and `lincheck::shared_lock` support for `defer_lock`, `try_to_lock`, `adopt_lock`, `mutex()`, `release()`, and `swap`.
- `lincheck::parker` one-permit park/unpark wrapper with managed scheduler blocking and traces.
- `lincheck::counting_semaphore<N>` and `lincheck::binary_semaphore` wrappers with real-thread acquire/release, managed scheduler permit queues, timed acquire expiration, and structured synchronization events.
- `lincheck::latch` wrapper with real-thread count-down/wait, managed scheduler wait queues, and structured synchronization events.
- `lincheck::barrier` wrapper with real-thread reusable phases, managed scheduler phase wait queues, arrive/drop support, and structured synchronization events.
- `lincheck::atomic<T>::wait`, `notify_one`, and `notify_all` for real threads and managed scheduler blocking traces.
- `lincheck::atomic_ref<T>` for instrumenting atomically accessed existing storage while preserving referenced-object IDs in events and managed atomic wait/notify keys.
- `lincheck::atomic<T>` single-order compare-exchange overloads derive the standard C++ failure order (`release` -> `relaxed`, `acq_rel` -> `acquire`) and trace the actual success/failure order pair.
- `lincheck::thread` runtime propagation, variadic callable arguments, `joinable()`, `get_id()`, `native_handle()`, `swap`, `detach()`, `lincheck::this_thread::get_id()`, `lincheck::this_thread::yield()`, start/finish/detach tracing, and child exception rethrow on `join()`.
- `lincheck::jthread` wrapper with runtime propagation, `std::stop_token` callable injection, request-stop helpers, auto-join semantics, and child exception rethrow on explicit `join()`.
- `lincheck::this_thread::sleep_for` and `sleep_until` wrappers that trace standard wait calls and expose scheduler switch points while avoiding wall-clock sleeps under the managed scheduler.
- `lincheck::stm` hook API for transaction begin/read/write/validate/lock/commit/abort/retry events, with per-thread transaction IDs and transaction depth in trace output.
- Optional STM opacity checking via `ModelCheckingOptions::check_opacity()` and `StressOptions::check_opacity()`, with value-history-only lifetime handling by default, opt-in `StmLifetimePolicy::strict_lifetimes`, structured `CheckResult::opacity_history`, `CheckResult::opacity_result`, `CheckResult::opacity_explanation`, raw-address lifetime generations in strict mode, explicit object-lifetime/field handles, ignored-lifetime anomaly diagnostics, read-from/order-prefix diagnostic samples, bounded committed-order search reporting, combined public-plus-opacity failure traces, and `FailureKind::opacity_violation`.
- STM hook traces preserve zero-valued clock, version, and lock-slot metadata instead of treating `0` as absent.
- STM value/location hooks for opacity: `tx_location_init`, `tx_location_register`, `tx_location_destroy`, `tx_read_value`, `tx_write_value`, and `tx_attempt_metadata`, including `LocationHandle` overloads, intrusive `tx_object<T>` / `tx_field<T>` helpers, and backend-facing `make_backend_object_lifetime_handle(...)`, `make_backend_location_handle(...)`, and `BackendLocationRegistry` helpers for explicit object-lifetime/field identity.
- STM opacity history JSON/DOT export helpers: `format_stm_opacity_history_json(...)` and `format_stm_opacity_history_dot(...)`.
- Multiverse hook binding header at `include/lincheck/multiverse_hooks.hpp`.
- Optional Multiverse hook call sites in `upstream/mvcc_tm/multiverse/multiverse.hpp`.
- Reproducible Multiverse hook patch at `third_party/mvcc_tm-lincheck-hooks.patch`.
- Multiverse-backed smoke workloads for counter, contended counter with retries, snapshot reads during retry contention, tiny-set, bank/account direct snapshot totals, bounded direct-snapshot updater detectors, shared-array ADTs, and invariant-preserving shared-array snapshot/updater scenarios, plus standalone tiny-set and bank/account examples.
- Runtime wrappers:
  - `lincheck::atomic<T>` with load, store, conversion/assignment operators, increment/decrement, arithmetic/bitwise compound operators, exchange, fetch_add, fetch_sub, fetch_and, fetch_or, fetch_xor, compare_exchange_strong, compare_exchange_weak, wait, notify_one, and notify_all
  - `lincheck::atomic_ref<T>` with matching atomic operation tracing over referenced storage
  - `lincheck::var<T>` with `get()`, `set()`, conversion, and assignment
  - `lincheck::mutex`
  - `lincheck::recursive_mutex`
  - `lincheck::timed_mutex`
  - `lincheck::recursive_timed_mutex`
  - `lincheck::shared_mutex`
  - `lincheck::shared_timed_mutex`
  - `lincheck::lock_guard`
  - `lincheck::scoped_lock`
  - `lincheck::unique_lock`
  - `lincheck::shared_lock`
  - `lincheck::condition_variable`
  - `lincheck::parker`
  - `lincheck::counting_semaphore<N>`
  - `lincheck::binary_semaphore`
  - `lincheck::latch`
  - `lincheck::barrier`
  - `lincheck::thread`
  - `lincheck::this_thread::get_id()`
  - `lincheck::this_thread::yield()`
  - `lincheck::switch_point()`
  - `lincheck::yield()`
  - `LC_READ(value)`
  - `LC_WRITE(target, value)`
  - `LC_CALL(name, fn)`
  - `LC_SWITCH()`
  - `LC_YIELD()`
  - `LC_THREAD_FENCE(order)`
  - `LC_SIGNAL_FENCE(order)`
- Reproducible model-checker traces containing the explored schedule.
- Programmatic deterministic replay through `CheckResult::schedule`, `schedule_from_trace(...)`, `schedule_decisions_from_trace(...)`, and `ModelCheckingOptions::replay(...)`.
- Observed scheduler-decision metadata through `CheckResult::schedule_decisions`, including switch-position indexes, source locations, and runnable thread choices used to expand the model-checker frontier.
- Typed `OperationContext` metadata on in-memory schedule decisions for runnable threads currently executing public ADT operations.
- Hand-authored scenario checking through `StressOptions::check(spec, scenario)` and `ModelCheckingOptions::check(spec, scenario)`.
- Hand-authored scenario validation for registered operation indexes, argument counts, one-shot operations, and non-parallel groups.
- Failure traces include a `failure:` summary with `CheckResult::failure` kind and message.
- Model-checker and stress failure traces include clock-sorted interleaving summaries, thread-by-thread interleaving tables, operation clocks, scenario sections, warnings, and optional state snapshots when the runner has that context.
- Runtime traces include registered operation start, finish, and throw events.
- Runtime wrapper traces include stable object IDs for atomic, plain `var`, mutex, condition-variable, and parker operations.
- Runtime trace inclusion/exclusion filters for model checking, stress, and `run_concurrent_test`.
- Optional state representation callbacks in failure reports.
- Verifier explanations for invalid linearizability results through `CheckResult::verifier_explanation` and failure trace sections.
- Structured failure taxonomy through `CheckResult::failure`.
- Named checks through `check(name, spec)` or `check(name, [] { return spec; })`, with names recorded in `CheckResult::test_name` and failure traces.
- Fail-fast validation for invalid runner counts, schedule bounds, and timeouts.
- Preserved `std::exception_ptr` on exception-driven failures through `CheckResult::exception`.
- Success and failure warnings through `CheckResult::warnings`, including clock-source caveats in failure traces.
- Non-`seq_cst` atomic and fence memory-order caveat warnings from explicit wrappers in model checks, stress checks, and `run_concurrent_test`.
- Explicit `memory_model(...)` option on stress and model-checking runners; only `MemoryModel::sequential_consistency` is supported today, and weak-memory modes fail fast.
- Model-checker exploration statistics through `CheckResult::stats`.
- Stable `lincheck::Value::stable_hash()` plus `lincheck::ValueHash` for hash-based result and argument sets.
- Greedy scenario minimization across init, parallel, and post sections for model-checker failures.
- Tests for:
  - broken load/store counter
  - deterministic stress-runner broken counter and single-slot queue overlaps
  - model-checker context-switch bounds
  - model-checker exploration statistics and budget pruning
  - source-macro instrumented load/store counter
  - correct atomic `fetch_add` counter
  - minimization of unneeded init/post actors
  - success-path clock-source warnings
  - process-global atomic clock sequencing
  - warning sections in failure traces
  - failure summary sections in model, stress, and concurrent-block traces
  - verifier explanation, operation-clock, and scenario sections in invalid-result traces
  - stress timeout, validation, and exception traces with operation-clock and scenario context
  - stress runtime trace filtering with and without timeout handling
  - non-`seq_cst` memory-order warnings on successful model, stress, and concurrent-block checks
  - trace inclusion/exclusion filters
  - structured failure kinds and stable failure-trace sections
  - operation start/finish/throw trace events
  - exception and deadlock traces with scenario and operation-clock sections
  - interleaving summary sections sorted by operation invocation clocks and grouped by thread
  - atomic exchange, arithmetic/bitwise fetch and operator conveniences, CAS overloads, and memory-order trace labels
  - thread/signal fence trace labels and source-location fence macros
  - stable object IDs in runtime wrapper traces
  - stable source macro object/location IDs
  - broken single-slot queue-like structure
  - obstruction-freedom success and switch-budget failure
  - obstruction-freedom handling for opt-in exception-as-result operations
  - model-checker livelock switch-budget failure
  - hand-authored stack and queue verifier histories
  - operation generator groups, non-parallel groups, one-shot operations, and trace metadata
  - per-scenario reset of stateful parameter generators
  - built-in and custom parameter generators
  - stable `Value` hashing
  - `std::optional<T>` and custom user-defined `Value` payloads and operation results
  - opt-in operation exceptions verified as operation results
  - custom object factories and sequential model cloners
  - typed callable operation registration
  - modular public include headers
  - invalid option validation
  - optional sequential-state verifier cache keys
  - stress and model-checker validation failures
  - stress and model-checker exception pointer preservation
  - stress-runner parallel worker start-gate alignment
  - cooperative model-checker peer abort after a primary failure
  - observed-frontier model-checker schedule traversal
  - cooperative stress-runner timeouts
  - real-time verifier ordering
  - `lock_guard` RAII/adopt-lock handling
  - `scoped_lock` variadic RAII/adopt-lock handling
  - `unique_lock` standard lock-tag constructors, ownership release, mutex access, and swap
  - managed mutex blocking/unblocking
  - deterministic scheduler golden trace output
  - captured model-check schedules replayed through `ModelCheckingOptions::replay(...)`
  - hand-authored scenario execution for stress and model-checker options
  - fail-fast validation for malformed hand-authored scenario actors, including one-shot and non-parallel-group constraint violations
  - condition-variable wait/notify on real and managed threads
  - condition-variable timed wait timeouts on real and managed threads
  - `lincheck::parker` park/unpark on real and managed threads
  - atomic wait/notify on real and managed threads
  - `lincheck::thread` runtime callback propagation, swap/native-handle support, `this_thread` helpers, and child exception rethrow
  - `lincheck::jthread` stop-token propagation, auto-join behavior, and child exception rethrow
  - `lincheck::this_thread` yield and sleep wrapper trace/switch-point behavior
  - source rewrite output compile/run smoke for a small rewritten threaded program
  - `run_concurrent_test` trace and exception reporting, including park/unpark traces
  - stable object IDs in managed lock, condition-variable, atomic wait, and parker traces
  - STM hooks and Multiverse hook macro binding, including validation failure, abort, retry, and lock release events
  - STM hook address formatting through stable object IDs
  - STM hooks as cooperative model-checker switch points
  - Multiverse-backed ADT failure traces with transaction and lock-release events
  - Multiverse-backed tiny-set model-check and stress workloads with generated key domains
  - Multiverse real-thread contention traces with transaction abort/retry events
  - `var`, `mutex`, and `thread` wrapper smoke paths

## Example

```cpp
struct BrokenCounter {
    lincheck::atomic<int> value{0};

    int inc() {
        const int observed = value.load();
        value.store(observed + 1);
        return observed + 1;
    }
};

struct SequentialCounter {
    int value = 0;
    int inc() { return ++value; }
};

lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
    .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

auto result = lincheck::ModelCheckingOptions()
    .iterations(1)
    .threads(2)
    .actors_per_thread(1)
    .max_schedule_length(3)
    .max_context_switches_per_schedule(2)
    .max_switch_points_per_schedule(10000)
    .check_obstruction_freedom()
    .obstruction_switch_bound(1000)
    .check("broken-counter", spec);

if (!result.success) {
    std::cerr << lincheck::failure_kind_name(result.failure) << ": "
              << result.message << "\n" << result.trace;
}
```

`CheckResult::failure` is `lincheck::FailureKind::none` on success. Defined failure kinds are `invalid_results`, `validation_failure`, `unexpected_exception`, `deadlock`, `livelock`, `obstruction_freedom`, and `timeout`.
Named checks store the supplied name in `CheckResult::test_name` and prefix non-empty failure traces with `test: <name>`.
Failure traces include a `failure:` section containing the same failure kind and message, so saved trace text can be read without the surrounding `CheckResult`.
When a failure is caused by an exception, `CheckResult::exception` preserves the original `std::exception_ptr` and `CheckResult::message` contains the formatted message. Operation-level exception failures still record the throwing operation's response clock and exception value in the operation-clock sections.
When a linearizability check rejects a history, `CheckResult::verifier_explanation` summarizes the failed search state, including rejected candidate operations, real-time-order blockers, or sequential return mismatches. Invalid-result traces include a `verifier explanation:` section.
Failure traces that have recorded operation intervals include an `interleaving:` section sorted by invocation/response clocks, a `thread interleaving:` section grouped by worker, and the fuller `operation clocks:` section grouped by scenario phase and thread.
Model-checker results store the executed schedule in `CheckResult::schedule`; `schedule_from_trace(result.trace)` can recover the same vector from saved trace text. The stored vector is the exact replay schedule consumed by the execution, including explicit keep-running choices after an exploratory prefix is exhausted. `CheckResult::schedule_decisions` records the switch-position index, source location, chosen thread, runnable alternatives, and in-memory `OperationContext` records for runnable threads that are inside public ADT operations at the decision. The model checker uses those decisions to expand unexplored schedule prefixes. Failure traces include a `schedule decisions:` section with the scheduling metadata and a compact `operations:` suffix when operation contexts are present; `schedule_decisions_from_trace(result.trace)` recovers the replay-critical scheduling fields while ignoring the compact operation suffix. `ModelCheckingOptions::replay(spec, result.scenario, result.schedule)` reruns by chosen-thread vector; `ModelCheckingOptions::replay(spec, result.scenario, result.schedule_decisions)` also validates the replayed switch positions, locations, and operation contexts when supplied in memory. Replay schedules must be non-empty, every thread choice must be valid for the supplied scenario, and replay rejects schedules that end early, choose a non-runnable thread, contain unused trailing choices, or carry stale decision metadata. `ModelCheckingOptions::check_schedule_prefix(...)` runs a discovered schedule as an initial prefix and then verifies the resulting public/opacity history without requiring exact schedule consumption; use it when corrected code may retry or drain after a broken-prefix reproducer. Model-checker failure traces also append a `model-checking stats:` section with explored/generated schedule counts, pruning counts, maximum explored context-switch depth, retained schedule length, retained context switches, and retained schedule-decision count. Use `format_model_checking_stats(result)` to emit the same section from caller-side debug or info logging.
For targeted checks, callers can pass a manually built scenario instead of using random generation:

```cpp
lincheck::ExecutionScenario scenario;
scenario.parallel = {
    {lincheck::Actor{.operation_index = 0, .name = "inc"}},
    {lincheck::Actor{.operation_index = 0, .name = "inc"}}
};

auto result = lincheck::ModelCheckingOptions().check(spec, scenario);
```

`CheckResult::warnings` may be populated on success or failure. For example, `ClockSourceKind::rdtsc` reports hardware and virtualization caveats there. Explicit atomic and fence wrappers also warn when they observe non-`seq_cst` `std::memory_order` values, because the current model checker records the labels but still explores wrapper behavior as sequentially consistent. The wrappers reject unknown memory-order enum values and atomic load/store/wait/CAS combinations that would be invalid for the corresponding C++ atomic operation before calling `std::atomic`. Runner options expose `memory_model(...)`; `MemoryModel::sequential_consistency` is the only supported mode today, while `cxx_release_acquire` and `cxx_relaxed` fail fast instead of silently claiming weak-memory exploration. Failure traces include a `warnings:` section when warnings are present.
`CheckResult::trace_events` contains generic runtime trace points observed during the retained invocation, schedule, or obstruction-freedom failure run, including operation start/finish, switch points, thread lifecycle events, and wrapper trace messages. Records carry a stable sequence number, unified `event_index`, thread ID, event kind, description, active public-operation context when available, and STM transaction ID/depth metadata for STM hook events and their scheduler switch points. Failure traces include a `trace events:` section when records are present. Stress and generated model-checking success results keep a representative successful trace-event stream; failures keep the failing run's stream.
`CheckResult::memory_events` contains structured atomic and fence event records observed during the retained invocation or schedule, including stable object IDs, source-location IDs cached when the event is recorded, active public-operation context when available, and a unified `event_index` shared with other structured event streams. Failure traces include a `memory events:` section when records are present. Stress and generated model-checking success results keep a representative successful event stream; failures keep the failing run's event stream.
`CheckResult::stm_events` contains retained STM hook records observed during the retained invocation or schedule, including stable address IDs, transaction IDs/depths, lock slots, versions, clocks, validation success flags, retry attempts, abort/retry reasons, active public-operation context when available, and the unified structured `event_index`. Failure traces include a `stm events:` section when records are present.
`CheckResult::source_accesses` contains retained source-level `LC_READ`/`LC_WRITE` records with stable object IDs, source-location IDs, captured values when they can be represented as a Lincheck `Value`, active public-operation context when available, and the unified structured `event_index`. Failure traces include a `source accesses:` section when records are present.
`CheckResult::synchronization_events` contains retained lock, wait, notify, park, and unpark records from Lincheck wrappers, including stable object IDs, related-object IDs such as a condition variable's lock, active public-operation context when available, and the unified structured `event_index`. Failure traces include a `synchronization events:` section when records are present.
`CheckResult::event_dependencies` summarizes the retained event streams as nodes and conservative edges. The current graph records same-thread and same-resource order within each stream, then uses the unified `event_index` to add cross-stream same-thread and same-resource edges for generic trace points, trace-level STM transaction IDs, stable object IDs, STM location-handle IDs, STM address or lock-slot IDs, source locations, and related synchronization objects. Dependency nodes inherit any active public-operation context attached to their source event, and JSON/DOT exports include that label. `CheckResult::operation_dependency_footprints` groups those labeled dependency nodes by public operation and records each operation's event-index range, event count, streams, and touched resources. `CheckResult::event_dependency_analysis` validates edge endpoints and reports whether the graph is acyclic, with a topological node order when one exists. Use `format_event_dependency_graph_json(...)` or `format_event_dependency_graph_dot(...)` to export the graph for offline inspection. Failure traces include `event dependencies:`, `event dependency analysis:`, and `operation dependency footprints:` sections when records are present. This is metadata for analysis and future POR; it does not add weak-memory exploration or infer a C++ memory-model order for uninstrumented events.
`ModelCheckingOptions::event_dependency_reduction()` lets observed-frontier traversal skip an alternative prefix when both the chosen and alternative runnable threads are inside public operations, the retained event dependency graph is consistent, both operations have dependency footprints, and those footprints touch disjoint shared resources. Transaction-local `tx#...` resources alone are not enough to prune. This is an opt-in SC reduction based on one observed schedule's retained wrapper events, not a full event-structure POR or weak-memory proof.
`CheckResult::stats` reports model-checker scenario and schedule counts, including generated frontier prefixes, explored schedules, context-bound pruning, invocation-budget pruning, opt-in operation-context pruning, opt-in event-dependency pruning, duplicate public-history verifier pruning, context-switch depth increases, and the maximum explored context-switch depth. When failure minimization is enabled, the returned failure and trace stats describe the retained minimized failure run, while `scenarios_generated` still records how many generated scenarios were tried before the failure returned.

Stress invocations can enforce a cooperative deadline when the tested code reaches Lincheck switch points:

```cpp
auto result = lincheck::StressOptions()
    .iterations(1)
    .invocations_per_iteration(10)
    .threads(2)
    .actors_per_thread(2)
    .invocation_timeout(std::chrono::milliseconds(100))
    .check(spec);
```

General-purpose concurrent blocks can be run with trace collection on real threads:

```cpp
auto result = lincheck::run_concurrent_test([&] {
    lincheck::thread t1([&] { counter.fetch_add(1); });
    lincheck::thread t2([&] { counter.fetch_add(1); });
    t1.join();
    t2.join();
    if (counter.load() != 2) throw std::runtime_error("counter mismatch");
});
```

This helper is not a linearizability verifier. It records events from Lincheck wrappers and reports exceptions through `CheckResult`.

Runtime trace output can be filtered with substring include/exclude rules. Filtering affects only recorded trace lines; scheduler switch points, timeouts, verification, operation clocks, warnings, state, and scenario sections are still evaluated.

```cpp
auto result = lincheck::ModelCheckingOptions()
    .trace_exclude("atomic.load")
    .check(spec);

lincheck::TraceFilter filter;
filter.include("operation.start");
auto block_result = lincheck::run_concurrent_test([&] {
    lincheck::thread worker([&] { counter.fetch_add(1); });
    worker.join();
}, filter);
```

Failure reports can include a user-defined snapshot of the concurrent object:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentMap, SequentialMap>()
    .operation("get", &ConcurrentMap::get, &SequentialMap::get, lincheck::range<int>(0, 9))
    .state_representation([](const ConcurrentMap& map) {
        return map.debug_string();
    });
```

On failure, `CheckResult::state_representation` contains the snapshot and the trace includes a `state:` section.

Verifier search can memoize repeated states when the sequential model exposes a stable key:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentCounter, SequentialCounter>()
    .operation("inc", &ConcurrentCounter::inc, &SequentialCounter::inc)
    .sequential_state([](const SequentialCounter& model) {
        return model.value;
    });
```

Without `sequential_state(...)`, the verifier keeps the conservative uncached DFS because arbitrary C++ model objects are opaque to the library. Post-actors are checked as part of the DFS terminal condition, so the verifier can backtrack from a parallel order that matches the parallel results but leaves the sequential model in a state that cannot explain the post operations.

Concurrent objects and sequential models are default-constructed unless factories are provided:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentCounter, SequentialCounter>()
    .concurrent_factory([] {
        return std::make_unique<ConcurrentCounter>(0);
    })
    .sequential_factory([] {
        return std::make_shared<SequentialCounter>(0);
    })
    .sequential_cloner([](const SequentialCounter& model) {
        return SequentialCounter(model);
    })
    .operation("inc", &ConcurrentCounter::inc, &SequentialCounter::inc);
```

Factory and cloner callables may return the object by value, `std::unique_ptr<T>`, or `std::shared_ptr<T>`. Sequential models must either be copy-constructible or provide `sequential_cloner(...)`.

Operations can also be registered as typed callables when member pointers are too restrictive:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentCounter, SequentialCounter>()
    .operation_callable<int>(
        "add",
        [](ConcurrentCounter& counter, int delta) {
            return counter.add(delta);
        },
        [](SequentialCounter* model, int delta) {
            return model->add(delta);
        },
        lincheck::values<int>({1, 2})
    );
```

The explicit template arguments on `operation_callable<Args...>` define how generated `lincheck::Value` arguments are cast before invoking the concurrent and sequential callables. A callable may take the object as `Obj&` or `Obj*`.

Operations and parameter domains may use `std::optional<T>` when `T` is equality-comparable, streamable, and hashable. Empty optionals are reported as `optional(nullopt)` and remain distinct from void operation results.

Concurrent-object invariants can be checked after a scenario finishes:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentCounter, SequentialCounter>()
    .operation("inc", &ConcurrentCounter::inc, &SequentialCounter::inc)
    .validation([](const ConcurrentCounter& counter) {
        return counter.debug_invariants_hold() ? std::string{} : "counter invariant failed";
    });
```

Validation callbacks may return `bool`, `std::string`/string-like messages, a `lincheck::Value`-convertible value, or `void` and throw on failure.

Operation constraints can be attached when registering an operation:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentMap, SequentialMap>()
    .operation("get", &ConcurrentMap::get, &SequentialMap::get, lincheck::range<int>(0, 9))
    .operation_with_options(
        "resize",
        &ConcurrentMap::resize,
        &SequentialMap::resize,
        lincheck::non_parallel_group("table-shape")
    )
    .operation_with_options(
        "initialize",
        &ConcurrentMap::initialize,
        &SequentialMap::initialize,
        lincheck::one_shot()
    )
    .operation_with_options(
        "size",
        &ConcurrentMap::size,
        &SequentialMap::size,
        lincheck::independent_operation_group("read-only-size")
    );
```

Generated scenario and operation trace labels include group and flag metadata, for example `resize() [group=table-shape non_parallel]`. A non-parallel group is generated in at most one parallel thread, where it may appear multiple times sequentially. A one-shot operation is generated at most once across init, parallel, and post sections. Hand-authored scenarios are validated against the same one-shot and non-parallel-group constraints before stress, model checking, replay, or standalone verification runs.

Operations in the same non-empty independence group are a stronger user contract: they assert that implementation-level interleavings between those operations can be treated as independent for schedule reduction. The model checker ignores this metadata by default. Calling `ModelCheckingOptions::operation_context_reduction()` lets observed-frontier traversal skip alternative prefixes where both the chosen and alternative runnable threads are currently inside operations from the same independence group. This is useful for explicitly independent read-only adapters, but it should not be used for operations whose implementation may communicate through shared state, caches, locks, retries, or hidden side effects.

`ModelCheckingOptions::event_dependency_reduction()` is a separate opt-in reduction that uses retained wrapper/STM/source/synchronization footprints instead of user-declared independence groups. It only prunes when both runnable operations have observed footprints and their shared-resource sets are disjoint. Keep it disabled for operations whose touched resources can change under a different interleaving unless you are using it as an exploratory reduction.

Operations that intentionally throw as part of the ADT contract can opt into comparing exceptions as operation results:

```cpp
lincheck::TestSpec spec = lincheck::test<ConcurrentQueue, SequentialQueue>()
    .operation_with_options(
        "pop",
        &ConcurrentQueue::pop,
        &SequentialQueue::pop,
        lincheck::exceptions_as_results()
    );
```

With this option, matching concurrent and sequential exceptions are formatted as `exception(<type>: <message>)` values and checked by the linearizability verifier. Operations without this option keep the default behavior: a throw is reported as an `unexpected_exception` failure.

Parameter generators are seeded through the scenario generator:

```cpp
enum class Color { red = 1, blue = 2 };

lincheck::TestSpec spec = lincheck::test<ConcurrentMap, SequentialMap>()
    .operation(
        "put",
        &ConcurrentMap::put,
        &SequentialMap::put,
        lincheck::gen<int>(),
        lincheck::strings({"left", "right"}),
        lincheck::values<Color>({Color::red, Color::blue}),
        lincheck::custom([](std::mt19937_64& rng) {
            return static_cast<int>(rng() % 100);
        })
    );
```

Use `gen<T>()` for small default domains for `bool`, enum types, signed and unsigned integer types, floating-point types, and `std::string`. Enum defaults use the enum's signed or unsigned underlying integer domain; use `values({Enum::a, Enum::b})` when a test needs only named enumerators or another precise domain.

Source macros can instrument code without changing fields to Lincheck wrapper types:

```cpp
struct Counter {
    int value = 0;

    int inc() {
        const int observed = LC_READ(value);
        const int next = LC_CALL("compute-next", [&] {
            return observed + 1;
        });
        LC_WRITE(value, next);
        LC_YIELD();
        return next;
    }
};
```

The macros record file, line, function metadata, and process-local stable object/location IDs in the trace. `LC_CALL(name, fn)` adds `call.begin`, `call.end`, and `call.throw` trace points around user-provided helper calls while preserving return values and exceptions; non-void `call.end` events include the returned value when it can be formatted as a Lincheck value. The macros are intended for explicitly model-checked code. In stress mode, raw shared variables are still ordinary C++ variables, so unprotected racy accesses are still undefined behavior.

To find raw synchronization APIs before a real Clang/LLVM instrumentation pass exists, build and run the source audit tool:

```bash
cmake --build build --target lincheck_source_audit
./build/lincheck_source_audit --exclude=build --exclude=upstream path/to/source
```

The audit is lexical and conservative. It reports raw APIs that usually need a Lincheck wrapper, source macro, or explicit adapter. Deliberate uses can be filtered with `--include=TEXT`, `--exclude=TEXT`, `--allow-token=TEXT`, `--allow-line=TEXT`, or inline suppressions such as `// NOLINT(lincheck-raw-sync)` and `// NOLINTNEXTLINE(lincheck-raw-sync)`.

When Clang/LLVM development packages are available, CMake can also build an AST-based audit prototype. It is opt-in through `-DLINCHECK_BUILD_CLANG_TOOLS=ON`:

```bash
cmake -S . -B build-clang-tools -DLINCHECK_BUILD_CLANG_TOOLS=ON
cmake --build build-clang-tools --target lincheck_clang_source_audit
./build-clang-tools/lincheck_clang_source_audit path/to/source.cpp -- -std=c++20 -Iinclude
```

The Clang audit currently checks main-file declarations and direct calls for raw standard atomics, threads, mutexes, condition variables, waits, semaphores, barriers, latches, and compiler atomic builtins. It honors the same `--include=TEXT`, `--exclude=TEXT`, `--allow-token=TEXT`, `--allow-line=TEXT`, `NOLINT(lincheck-raw-sync)`, and `NOLINTNEXTLINE(lincheck-raw-sync)` filters as the lexical audit. It is a source-audit bridge, not a transparent instrumentation pass or full LibTooling rewriter.

For small files that use common standard wrappers, `lincheck_source_rewrite` can produce an instrumented copy:

```bash
cmake --build build --target lincheck_source_rewrite
./build/lincheck_source_rewrite path/to/source.cpp > path/to/source.lincheck.cpp
./build/lincheck_source_rewrite --check path/to/source.cpp
./build/lincheck_source_rewrite --check path/to/source-tree
./build/lincheck_source_rewrite --in-place path/to/source-tree
```

The rewriter is also lexical. It rewrites supported tokens such as `std::atomic`, `std::atomic_ref`, `std::thread`, `std::jthread`, `std::mutex`, `std::recursive_mutex`, `std::timed_mutex`, `std::recursive_timed_mutex`, `std::shared_mutex`, `std::shared_timed_mutex`, `std::shared_lock`, `std::condition_variable`, `std::binary_semaphore`, `std::counting_semaphore`, `std::barrier`, `std::latch`, `std::lock_guard<std::mutex>`, `std::unique_lock<std::mutex>`, `std::this_thread::yield`, `std::this_thread::sleep_for`, `std::this_thread::sleep_until`, and standard atomic fence calls while leaving comments, string literals, and suppressed lines alone. Directory inputs are supported for `--check` and `--in-place`; stdout and `--output=FILE` modes require exactly one source file input. A CTest smoke rewrites, compiles, and runs a small threaded fixture, but this is still a convenience bridge for explicitly instrumented tests, not a whole-program C++ parser.

`lincheck::parker` provides a small one-permit parking primitive for tests that need explicit park/unpark behavior:

```cpp
lincheck::parker gate;

lincheck::thread worker([&] {
    gate.park();
    critical_step();
});

gate.unpark();
worker.join();
```

Under the cooperative scheduler, `park()` blocks the current managed thread until `unpark()` releases a waiter or stores a permit. On ordinary real threads it uses a condition variable internally.

## Transactional Memory

Lincheck++ has a small generic STM hook layer in `lincheck::stm`. An STM backend can call lifecycle hooks such as `tx_begin`, metadata-only access hooks such as `tx_read` and `tx_write`, value-carrying access hooks such as `tx_read_value` and `tx_write_value`, location hooks such as `tx_location_init`, `tx_location_register`, and `tx_location_destroy`, and commit/conflict hooks such as `tx_validate_begin`, `tx_validate_end`, `tx_lock_attempt`, `tx_lock_acquired`, `tx_lock_failed`, `tx_lock_released`, `tx_commit_attempt`, `tx_commit_success`, `tx_abort`, `tx_retry`, and `tx_attempt_metadata` from its transaction machinery. When a Lincheck runtime is active, each hook becomes a structured `CheckResult::stm_events` record, a generic trace event with transaction metadata, and a cooperative model-checker switch point.

The default intended use is still to check a linearizable ADT implemented over transactional memory. Register the ADT's public operations and a sequential model exactly as with a non-STM object; the public verifier checks operation results, exceptions-as-results, real-time ordering, post operations, and optional validation/state callbacks.

When `check_opacity()` is enabled, Lincheck additionally builds an STM transaction history from value/location hooks and checks opacity separately from ADT linearizability. The default lifetime policy is `StmLifetimePolicy::value_history_only`: raw addresses are treated as logical value-history locations, raw-address destroy/reuse/use-after-destroy anomalies do not make history construction fail, and the builder records ignored-lifetime anomaly counters/samples instead. This deliberately leaves memory reclamation and reuse-after-free correctness out of scope by default; if a bad reclaimed value later appears in a transaction read or public operation result, the normal opacity/public linearizability checks can still reject the retained execution.

Use `.opacity_lifetime_policy(lincheck::StmLifetimePolicy::strict_lifetimes)` on `ModelCheckingOptions` or `StressOptions`, or pass `lincheck::StmOpacityHistoryBuildOptions{.lifetime_policy = lincheck::StmLifetimePolicy::strict_lifetimes}` to `build_stm_opacity_history(...)`, when the test is explicitly checking lifecycle/reclamation instrumentation. In strict mode, raw-address reuse after `tx_location_destroy(address)` is represented as a new location generation only across a quiescent boundary where no transaction is live; destroyed raw-location access, raw destroy/reuse while transactions are live, ambiguous raw/handle reuse, and missing disambiguating handles are malformed histories.

The opacity verifier itself is unchanged by the lifetime policy. It asks whether committed transaction attempts can be serialized, and whether aborted/live attempts can be placed as observers of a consistent serial prefix while respecting visible real-time order between transactions. Reads must observe a prior write in the same transaction, the latest preceding committed write, or a registered initial value. Before committed-order backtracking, the verifier builds a conservative ordering graph from visible real-time edges and sound read-from constraints; graph cycles are rejected immediately, blocked candidates are skipped until their required predecessors are placed, ambiguous sources forced after the reader are eliminated, ambiguous sources forced before a conflicting writer that must also precede the reader are eliminated, matching initial sources are eliminated when a conflicting writer must precede the reader, non-initial ambiguous reads with one viable source left are promoted to a normal read-from edge, ambiguous non-initial readers are skipped until at least one possible source writer is placed, and ambiguous reads are skipped when the current prefix's latest placed writer to that location conflicts with the observed value. Malformed hook streams, missing initial values, unsupported value payloads, strict lifetime failures, value opacity violations, and search-limit exhaustion fail explicitly as `FailureKind::opacity_violation`; the detailed status is in `CheckResult::opacity_result`.

For a new STM backend, place hooks at transaction lifecycle and conflict points that can affect public operation outcomes: transaction begin, value-bearing read/write barriers, validation start/end, lock acquisition attempts and outcomes, lock release, commit attempt/success, abort, and retry. Preserve stable addresses, lock slots, versions, clocks, retry attempts, and abort reasons when the STM has them; zero-valued clocks, versions, and lock slots are valid metadata and are intentionally retained. Opacity mode requires `tx_location_init` before any checked location is read/written, and requires `tx_read_value`/`tx_write_value` instead of metadata-only `tx_read`/`tx_write` for checked accesses.

For tests or adapters that can opt in, explicit handles avoid guessing ownership from raw field addresses. `lincheck::stm::tx_object<T>` / `lincheck::tx_object<T>` creates an object lifetime handle, and `lincheck::stm::tx_field<T>` / `lincheck::tx_field<T>` stores a location handle made from that object lifetime plus a field ID:

```cpp
struct Node : lincheck::tx_object<Node> {
    lincheck::tx_field<int> value;

    explicit Node(int initial)
        : value(lc_object_handle(), "value", initial) {}
};
```

Standalone `tx_field<T>` instances are self-owned: the field object itself is treated as the allocation unit. The helper layer emits handle-based register/init/read/write/destroy hooks when a Lincheck runtime is active. Backends can also bypass the intrusive helper and call the `LocationHandle` overloads directly, or use `make_backend_object_lifetime_handle(...)`, `make_backend_location_handle(...)`, and `BackendLocationRegistry` when the STM can provide an allocation token, object pointer, generation, field ID, field address, and diagnostic label/type name. `BackendLocationRegistry` assigns conservative generations per backend allocation token and is useful for adapter code that sees construction/destruction but does not own the C++ type. Raw-address hooks still work for ordinary value-history opacity checking; strict generation/reclamation semantics are available only when the strict lifetime policy is selected.

Committed transaction ordering is searched with conservative read-from pruning, ordering-graph pruning, ambiguous read-from prefix pruning, and failed-prefix memoization by default for the retained history. The read-from pruning adds non-disjunctive edges for unique non-initial committed writers and initial-value reads with no matching committed writer. Ambiguous reads record a disjunctive source choice: sources that must occur after the reader are eliminated, sources shadowed by forced conflicting writers are eliminated, matching initial sources shadowed by forced conflicting writers are eliminated, a non-initial read with one viable source left is promoted to a normal edge, non-initial ambiguous reads require at least one possible committed source before the reader, and all ambiguous reads are skipped when the prefix's latest placed writer to the location is a conflicting writer. Ambiguous cases that survive those checks are left to serial-state search. The core test suite includes deterministic small/medium generated histories for larger ambiguous source spaces, multi-reader ambiguity, retry-heavy logical attempts, longer observer chains, search-limit observers, memoization pressure, and paired possible/impossible cycle-shaped histories. For larger histories, set `ModelCheckingOptions::opacity_max_committed_orders(n)` or `StressOptions::opacity_max_committed_orders(n)` to report `StmOpacityStatus::search_limit_exceeded` instead of spending unbounded time in the opacity verifier. Failure traces include an `stm opacity:` section with lifetime policy, ignored lifetime anomaly count/samples, committed transaction count, observer transaction count, a factorial upper bound for the committed-order search space, the configured search limit, explored committed orders, memo entries, memo-pruned prefixes, read-from constraints, ambiguous read-from constraints/prunes, source-before-reader prunes, conflicting-writer prunes, eliminated ambiguous sources, shadowed ambiguous sources, eliminated matching initial sources, promoted ambiguous constraints, ordering-graph edge/prune counts, observer checks, bounded read-from witnesses, ordering-graph samples, rejected prefix samples, locations with raw address/generation and explicit handle/object/field metadata, transactions, and access values. If public ADT verification and opacity both fail for the same retained run, the trace includes both the verifier explanation and the opacity section. `format_stm_opacity_history_json(...)` and `format_stm_opacity_history_dot(...)` export the retained opacity history and optional verification result for offline inspection.

To model and reject reclamation or address-reuse bugs directly, use strict lifetime policy and ensure every non-quiescent lifecycle/read/write event for reused addresses carries stable identity, such as an allocation/object lifetime token plus field ID or a per-access `LocationHandle`. Raw and handle-based locations can appear in one history when they identify distinct locations. In default value-history mode, those same raw lifetime anomalies are diagnostics only.

### Multiverse Adapter

To bind the optional Multiverse hook call sites to Lincheck STM trace events, include the binding header before including Multiverse:

```cpp
#include <lincheck/multiverse_hooks.hpp>
#include "multiverse/multiverse.hpp"
```

The Multiverse integration is an adapter/example backend, not part of the core opacity verifier. The hook macros are no-ops when the binding header is not included. When bound to Lincheck, STM event traces, STM scheduler switch-point trace records, and `CheckResult::stm_events` include `tx_id=<n>`, `tx_depth=<n>`, value, handle-based location identity, logical transaction ID, and attempt metadata where the backend supplies it. The current adapter models each Multiverse `tx_field<T>` as a self-owned allocation unit keyed by the field object pointer, so lifecycle and value hooks carry an object-lifetime ID, generation, field ID `self`, and raw field address. Generic raw-address hook macros remain as fallbacks for paths that cannot safely provide a stable field object token. This lets opacity failures correlate transaction attempts with public operations and retained STM events.

The current Multiverse smoke tests include a small STM-backed set with `add`, `remove`, and `contains` operations registered against a `std::set` sequential model, a contended counter with forced retry observers, snapshot reads during retry contention, a multi-field bank/account ADT whose transfers update two transactional fields in one public operation, read-only/read-heavy bank snapshot scenarios, direct `TX_IS_SNAPSHOT` bank total scenarios with concurrent updaters, a same-address `tx_field<T>` construction/destruction reuse check for handle metadata, and shared-array ADTs. Direct-snapshot updater stress tests are detector-style application coverage: a retained run may pass, or it may report a public/opacity violation, but timeout/deadlock/crash is still a test failure. One shared-array workload picks a slot set and integer delta, checks the final array sum against remembered per-operation contributions, and keeps a deliberately tiny bounded operation set so this remains a smoke test instead of a retry-storm benchmark. Another invariant-preserving shared-array scenario transfers value between slots while a snapshot reader sums the whole array; the direct-snapshot bounded stress variant uses a retry-safe accumulator and is expected to pass, while the default Mode2 variants enter Mode2 through Multiverse's transition protocol before running snapshot/updater operations. A manual direct-forced Mode2 detector intentionally uses a test-only mode switch so the model checker can find the known snapshot/updater bug on the broken hooked checkout; `tools/check_multiverse_mode2_algorithm_fix.sh` applies the separate optional algorithm patch, checks the discovered schedule prefix against the fixed checkout, and reverses the patch before exit. The intentional missing-slot variant is checked both as a final validation failure and, with validation disabled plus a registered post `sum()` operation, as an `invalid_results` linearizability failure. High-overlap parameters can produce very high transactional abort and retry rates, and may look like deadlock or livelock unless slot count, selected-slot count, thread count, iteration count, invocation timeout, and opacity committed-order limit are chosen with expected contention in mind. Internal Multiverse mode2 transitions are controlled only by adapter/application setup helpers rather than core Lincheck++ behavior. The pattern is:

```cpp
#include <lincheck/multiverse_hooks.hpp>
#include "multiverse/multiverse.hpp"

struct TxSet {
    ns_multiverse::tx_field<int> slot;

    bool add(int key) {
        bool inserted = false;
        ns_multiverse::updateTx([&] {
            if (slot.load() == key) return;
            slot.store(key);
            inserted = true;
        });
        return inserted;
    }
};

struct SeqSet {
    std::set<int> keys;
    bool add(int key) { return keys.insert(key).second; }
};

auto spec = lincheck::test<TxSet, SeqSet>()
    .operation("add", &TxSet::add, &SeqSet::add, lincheck::values<int>({1, 2, 3}));
```

Without `check_opacity()`, Lincheck verifies only the public ADT operation history. With `check_opacity()`, Multiverse hook events are also checked for the bounded opacity property described above. This is still not a weak-memory proof and does not say anything about unhooked STM internals.

For complete runnable versions of this pattern, see `examples/multiverse_tiny_set_example.cpp` and `examples/multiverse_bank_example.cpp`. Each performs both a bounded model check over a representative scenario and a stress check over generated workloads. The shared-array workload currently lives in `tests/multiverse_smoke_tests.cpp`.

For an already patched checkout, `bash tools/check_multiverse_patch.sh` validates that `third_party/mvcc_tm-lincheck-hooks.patch` can be reversed and exactly matches the checkout diff. You can pass alternate paths as `bash tools/check_multiverse_patch.sh <mvcc_dir> <patch_file>`. The optional `bash tools/check_multiverse_mode2_algorithm_fix.sh` routine starts from that broken hooked checkout, demonstrates the model-checker-found Mode2 bug, temporarily applies `third_party/mvcc_tm-mode2-algorithm-fixes.patch`, verifies the discovered prefix no longer fails, and restores the broken hooked checkout.

## Current Limits

- No transparent compiler, LLVM, or Clang instrumentation.
- `lincheck_source_audit` can flag likely uninstrumented synchronization APIs, including standard mutex families, shared mutexes, semaphores, barriers, and latches. `lincheck_clang_source_audit` adds an optional AST-based audit prototype for raw standard synchronization APIs when LibTooling is available. `lincheck_source_rewrite` can lexically rewrite a small supported subset including standard mutexes, recursive mutexes, timed mutexes, shared mutexes, semaphores, barriers, and latches, but none of these tools provides whole-program instrumentation.
- No fibers or coroutines.
- No JVM compatibility.
- No C++ weak-memory exploration beyond sequentially consistent wrapper behavior.
- `memory_model(...)` intentionally rejects the currently unimplemented weak-memory modes.
- Atomic and fence traces include requested `std::memory_order` labels and return caveat warnings for non-`seq_cst` orders, but the model checker still explores wrapper behavior as sequentially consistent.
- Explicit atomic and fence wrappers reject unknown memory-order enum values, and atomic load/store/wait/CAS reject C++-invalid order combinations before delegating to `std::atomic`.
- The model checker controls only operations that reach Lincheck switch points.
- Model-checker abort is cooperative. Peer workers stop when they next enter a Lincheck runtime callback; uninstrumented loops are not interrupted.
- Model-checker livelock detection is cooperative and trips only when code keeps reaching Lincheck switch points after the schedule switch budget is exhausted.
- Stress timeouts are cooperative. They are reported when instrumented code reaches Lincheck switch points after the deadline; an uninstrumented infinite loop can still hang a native worker thread.
- Obstruction-freedom checking is cooperative. It detects operations that keep reaching Lincheck switch points without completing; it cannot interrupt a tight uninstrumented infinite loop.
- `rdtsc`/`rdtscp` clocks can be inconsistent across sockets, NUMA nodes, virtual machines, older CPUs without invariant synchronized TSC, and aggressive power-management settings. Use the atomic-sequence clock when strict cross-thread ordering matters.
- STM opacity checking is opt-in and bounded by visible hooks and schedule/stress coverage. It assumes sequential consistency and does not check unhooked internal STM state, privatization safety, open/closed nesting semantics, or weak-memory behavior. Reclamation/address-reuse bugs are out of scope in the default value-history policy and require opt-in strict lifetime instrumentation.
- Platform wait primitives and raw standard-library waits outside the Lincheck wrappers are not modeled yet.
- Parking is modeled only through `lincheck::parker`, and atomic wait/notify is modeled only through `lincheck::atomic<T>` and `lincheck::atomic_ref<T>`; raw OS futexes, raw `std::atomic::wait`, raw `std::atomic_ref::wait`, and third-party park APIs still need wrappers or instrumentation.
- `lincheck::test<Concurrent, Sequential>()` can use custom factories for non-default-constructible types. Sequential specifications still need copy snapshots, either through a copy constructor or `sequential_cloner(...)`.

These limits match the current MVP scope; they are not intended to be hidden compatibility claims.
