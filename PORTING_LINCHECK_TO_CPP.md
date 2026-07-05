# Plan to Port JVM Lincheck to C++

Date: 2026-07-03

## Source Downloaded

The JVM source was downloaded from the official JetBrains repository:

- Upstream: https://github.com/JetBrains/lincheck
- Local checkout: `upstream/lincheck-jvm`
- Version: `lincheck-3.6`
- Commit: `934408d`
- Commit date: 2026-05-26
- License: repository root is MPL 2.0. Several source files still carry LGPL-style headers, especially older utility and event-structure files, so do a license review before copying code verbatim into a C++ tree.

The Multiverse/MVCC TM source was also downloaded as an editable target:

- Upstream: https://gitlab.com/Coccimiglio/mvcc_tm
- Local checkout: `upstream/mvcc_tm`
- Branch: `master`
- Commit: `0d1454c`
- Commit date: 2026-06-29
- Commit message: `Fixed edge case in unversioning`
- Submodules initialized recursively, including `setbench`, `setbench/common/recordmgr`, and `setbench/tools`.
- Local hook instrumentation is captured as `third_party/mvcc_tm-lincheck-hooks.patch` so a fresh checkout or CI job can recreate the editable integration target.

Useful external references:

- GitHub repository: https://github.com/JetBrains/lincheck
- Kotlin Lincheck guide: https://kotlinlang.org/docs/lincheck-guide.html
- JetBrains project page: https://lp.jetbrains.com/research/concurrent-computing-lab/projects/lincheck/

## What Lincheck Is

Lincheck is a JVM framework for testing concurrent code. It has two user-facing modes:

- Stress testing: generate scenarios, run them many times on native threads, and hope the OS scheduler exposes a bad interleaving.
- Bounded model checking: transform bytecode to insert switch points, control scheduling deterministically, explore interleavings, and print a reproducible trace.

It also has a data-structure testing API where users declare operations, optionally provide a sequential specification, and Lincheck verifies observed concurrent results for linearizability by default.

## JVM Source Layout

The downloaded repository is organized as follows:

| JVM module | Purpose | C++ port decision |
|---|---|---|
| `lincheck/` | Public API, scenario generation, runners, strategies, verifiers, trace reporting | Port core ideas and algorithms |
| `bootstrap/` | Java classes loaded into the bootstrap classloader, including `EventTracker`, `Injections`, `TestThread`, `ThreadDescriptor` | Redesign as C++ runtime callbacks and thread registry |
| `jvm-agent/` | ByteBuddy/ASM agent that instruments bytecode for reads, writes, thread events, monitors, parking, loops, calls | Do not mechanically port. Replace with explicit C++ wrappers first, optional LLVM/Clang instrumentation later |
| `common/` | Descriptors, logging, object graph helpers, atomic method metadata, trace context | Partially port as runtime metadata and utilities |
| `trace/` | Binary trace format, trace reader/writer, trace printing and diffing | Port text trace first, binary trace later |
| `tracer/` | Standalone trace collection agent | Defer |
| `trace-recorder/` | Trace recording JVM agent | Defer |
| `live-debugger/` | IntelliJ live debugging agent | Do not port initially |
| `integration-test/` | Integration tests and real-world recordings | Mine for test cases after MVP |

Approximate source file counts under `src`: `lincheck` 297 JVM source files, `jvm-agent` 85, `common` 33, `trace` 36, `bootstrap` 10.

## Key JVM Components to Recreate

Core files inspected:

- Public API: `lincheck/src/jvm/main/org/jetbrains/lincheck/Lincheck.kt`
- Data-structure API: `lincheck/src/jvm/main/org/jetbrains/lincheck/datastructures/Options.kt`, `StressOptions.kt`, `ModelCheckingOptions.kt`
- Test discovery: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/CTestStructure.java`
- Scenario model: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/execution/ExecutionScenario.kt`
- Scenario generator: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/execution/RandomExecutionGenerator.java`
- Strategy loop: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/strategy/Strategy.kt`
- Stress strategy: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/strategy/stress/StressStrategy.kt`
- Managed scheduler: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/strategy/managed/ManagedStrategy.kt`, `ManagedThreadScheduler.kt`
- Interleaving search: `lincheck/src/jvm/main/org/jetbrains/kotlinx/lincheck/strategy/managed/modelchecking/ModelCheckingStrategy.kt`
- Verifier: `lincheck/src/jvm/main/org/jetbrains/lincheck/datastructures/verifier/Verifier.java`, `LinearizabilityVerifier.kt`, `AbstractLTSVerifier.kt`, `LTS.kt`
- Runtime callbacks: `bootstrap/src/sun/nio/ch/lincheck/EventTracker.java`, `Injections.java`, `TestThread.java`, `ThreadDescriptor.java`
- Bytecode event insertion: `jvm-agent/src/main/org/jetbrains/lincheck/jvm/agent/transformers/SharedMemoryAccessTransformer.kt`, `ThreadTransformers.kt`

## Porting Strategy

Do not attempt a line-by-line Kotlin or Java translation. The JVM implementation relies on runtime bytecode transformation, reflection, classloaders, Java object identity, Java monitors, Kotlin coroutines, and JVM stack metadata. A practical C++ port should preserve behavior and algorithms where they transfer, but redesign instrumentation and API boundaries for C++.

The first C++ version should be a library for tests, not a transparent whole-program instrumenter.

### MVP Scope

Ship these first:

- Data-structure testing API.
- Explicit operation registration instead of reflection annotations.
- Random scenario generation.
- Stress testing on `std::thread` or a fixed thread pool.
- Linearizability verifier with a user-provided sequential model.
- Cooperative model checker with explicit switch points.
- Text trace output for failing schedules.
- Scenario minimization.

Defer these:

- Transparent instrumentation of arbitrary reads and writes.
- Kotlin coroutine support.
- JVM-style reflection and annotations.
- Java agent, bootstrap classloader behavior, ByteBuddy, ASM.
- IntelliJ live debugger and trace recorder.
- Event-structure partial order reduction.
- Full C++ release/acquire memory model exploration.

Workspace boundary for now:

- Do not modify files outside `/home/trbot/agents/tmdb-collab-runtime/lincheck`.
- It is okay to read external target code such as `tmdb/util/sync` for context.
- The editable Multiverse checkout at `upstream/mvcc_tm` is inside this workspace and may be modified when the Lincheck integration requires it.

Near-term STM support is in scope after the MVP:

- Operation start/finish clocks so the verifier can enforce real-time order, not only per-thread order.
- Cooperative lock support good enough for model checking blocking and contended critical sections.
- STM transaction hooks that expose begin, read, write, validate, commit, abort, retry, and lock-word events as switch points.
- A Multiverse integration target for `upstream/mvcc_tm`, starting with an adapter where possible and adding direct hooks in the editable checkout where useful.
- Verification at the public ADT operation boundary. The checker should use STM internals to find interleavings, but it should still verify only the registered object's operation return values against the sequential specification.

## Proposed C++ Architecture

### Public API

Expose an explicit C++ DSL:

```cpp
lincheck::ModelCheckingOptions{}
    .threads(2)
    .actors_per_thread(5)
    .iterations(100)
    .invocations_per_iteration(10000)
    .check("msqueue", [&] {
        return lincheck::test<ConcurrentQueue, SequentialQueue>()
            .operation("push", &ConcurrentQueue::push, &SequentialQueue::push, lincheck::gen<int>())
            .operation("pop", &ConcurrentQueue::pop, &SequentialQueue::pop);
    });
```

Also support a general-purpose concurrent block later:

```cpp
lincheck::run_concurrent_test([&] {
    auto t1 = lincheck::thread([&] { counter.store(counter.load() + 1); });
    auto t2 = lincheck::thread([&] { counter.store(counter.load() + 1); });
    t1.join();
    t2.join();
    CHECK(counter.load() == 2);
});
```

For the MVP, make it explicit that general-purpose model checking only sees operations that use `lincheck::thread`, `lincheck::atomic`, `lincheck::var`, `lincheck::mutex`, and explicit `lincheck::yield()` or `lincheck::switch_point()` calls.

### Runtime Data Model

Port these conceptual models:

- `Actor`: operation name, generated arguments, operation flags, and invocation function.
- `ExecutionScenario`: init section, parallel per-thread operations, post section, optional validation.
- `ExecutionResult`: per-thread result values, exceptions, and happens-before clocks where needed.
- `OperationInterval`: invocation clock, response clock, thread id, actor id, and returned value.
- `LincheckFailure`: incorrect result, timeout, deadlock, livelock, validation failure, unexpected exception.
- `TracePoint`: switch, read, write, call, lock, unlock, park, unpark, operation start/end.

C++ result values should use a type-erased comparable value:

- Start with `std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string>`.
- Add user extension via `lincheck::Value` with equality, string formatting, and stable hashing.
- Provide first-class `std::optional<T>` adaptation for common queue/map operation results.
- Represent unexpected checker failures as `std::exception_ptr` plus formatted type/message, and support opt-in operation exceptions as comparable operation results.

### Scenario Generation

Port `RandomExecutionGenerator` directly in spirit:

- Generate `actors_before`, parallel actors per thread, and `actors_after`.
- Allow callers to bypass generation by passing an explicit `ExecutionScenario` to stress or model-checking options.
- Support operation groups and non-parallel groups.
- Generate non-parallel groups in at most one parallel thread while allowing repeated sequential actors in that thread.
- Support one-shot operations.
- Validate explicit hand-authored scenarios against registered operation arity, one-shot limits, and non-parallel group placement.
- Seed all random sources deterministically.
- Reset parameter generators per scenario like the JVM implementation does.

Parameter generators:

- Built-ins: `gen<T>()` defaults for common scalar/string types, bool, signed/unsigned integers, enum underlying-value domains, strings, fixed set, range.
- Custom: any callable returning a value from a seeded `Random`.

### Verification

Port `Verifier`, `AbstractLTSVerifier`, and `LinearizabilityVerifier` as a central, reusable component.

Recommended C++ design:

- Each operation registers two callables:
  - concurrent callable: mutates the tested concurrent object.
  - sequential callable: mutates a sequential model.
- The verifier DFS explores legal sequential orders respecting per-thread order and recorded operation clocks.
- The verifier must enforce real-time order: if operation A's response clock is before operation B's invocation clock, then A must precede B in every candidate sequential history.
- Post-section operations must be checked as part of the DFS terminal condition so the verifier can backtrack from a parallel linearization that matches parallel return values but leaves the sequential model in the wrong state for post operations.
- Each DFS state owns a sequential model snapshot.
- Require the sequential model to be copyable for MVP. Later add undo logs for performance.
- The current `lincheck::test<Concurrent, Sequential>()` DSL supports member-function operation registration, typed concurrent/sequential operation callables, and custom concurrent/sequential factories. Sequential models still need copy snapshots, either through a copy constructor or a custom sequential cloner.

Verifier milestones:

1. Linearizability only.
2. Operation interval clocks and non-overlap ordering.
3. Cache failed and successful verifier states.
4. Add serializability or quiescent consistency if needed.
5. Add suspension/cancellation analogues only if the C++ API grows coroutine support.

Clock source requirements:

- Provide a `ClockSource` abstraction used by the runner to stamp operation invocation and response.
- Start with an `rdtsc`/`rdtscp` implementation if available, because it is cheap and good enough for empirical private-use runs.
- Also provide a strict fallback based on a global atomic sequence counter for platforms where TSC ordering is suspect.
- Warn users that raw TSC values can be inconsistent across sockets, NUMA nodes, virtualized environments, older CPUs without invariant/synchronized TSC, and under aggressive power-management settings. In those environments, `rdtsc` clocks are evidence for exploration and reporting, not a proof-grade total order unless the machine's TSC behavior is known.

### Stress Strategy

Stress mode should be simple:

- Pre-spawn worker threads or create `std::jthread`s per invocation.
- Use barriers/latches to align parallel operation starts.
- Run each generated scenario many times.
- Collect result values and opt-in operation exceptions.
- Apply the same verifier.
- Detect timeout by invocation deadline.

Stress mode does not need memory instrumentation for MVP.

### Model Checking Strategy

Port the interleaving-tree algorithm from `ModelCheckingStrategy`:

- Maintain a tree alternating switch-position choices and next-thread choices.
- Increase allowed context-switch depth when the current depth is exhausted.
- Use deterministic random weighting to spread exploration across unexplored nodes.
- The current C++ model checker uses an observed-frontier traversal: it runs a schedule prefix, records switch-position indexes, source locations, runnable choices, and chosen threads at each explicit scheduler decision, and enqueues unexplored alternative prefixes up to `max_schedule_length`. It exhausts shallower context-switch depths before raising the allowed depth, with `max_context_switches_per_schedule(...)` acting as a hard cap. `seed(0)` keeps FIFO/lexicographic traversal, while nonzero `seed(...)` values deterministically weight same-depth frontier prefixes and perturb runnable-choice order reproducibly.
- Reproduce an interleaving by replaying switch positions and chosen threads through the exact executed schedule stored in `CheckResult::schedule`, `CheckResult::schedule_decisions`, `schedule_from_trace(...)`, `schedule_decisions_from_trace(...)`, and `ModelCheckingOptions::replay(...)`.
- Validate replay schedules against the supplied scenario and runtime consumption so stale or edited traces fail clearly when they end early, choose a non-runnable thread, or contain unused trailing choices.
- Collect a trace on replay after a failure.

Replace JVM bytecode switch-point insertion with cooperative switch points:

- `lincheck::switch_point(location)` for explicit user points.
- `lincheck::var<T>` for tracked plain reads/writes, including conversion and assignment conveniences.
- `lincheck::atomic<T>` for tracked atomic load/store/CAS/fetch/wait/notify operations and common C++ atomic operator conveniences.
- `lincheck::atomic_ref<T>` for tracked atomic load/store/CAS/fetch/wait/notify operations over existing storage, preserving the referenced object as the event identity.
- Atomic compare-exchange wrappers should match the standard overload semantics, including derived failure orders for the single-order overloads, while still warning that the current checker explores only sequentially-consistent wrapper behavior.
- `lincheck::mutex`, `lincheck::recursive_mutex`, `lincheck::timed_mutex`, `lincheck::recursive_timed_mutex`, `lincheck::shared_mutex`, `lincheck::shared_timed_mutex`, `lincheck::lock_guard`, `lincheck::scoped_lock`, `lincheck::unique_lock`, and `lincheck::shared_lock`.
- `lincheck::condition_variable`.
- `lincheck::parker` for explicit park/unpark points.
- `lincheck::counting_semaphore<N>` and `lincheck::binary_semaphore` for explicit semaphore acquire/release points.
- `lincheck::latch` for one-shot count-down/wait points.
- `lincheck::barrier` for reusable arrive/wait/drop phase points.
- `lincheck::thread`.

This avoids C++ undefined behavior from uninstrumented data races. For model checking, plain shared state must be accessed through Lincheck wrappers or user-provided callbacks.

### Scheduler Runtime

Implement the equivalent of `ManagedThreadScheduler`:

- Register worker threads with stable integer IDs.
- Allow exactly one scheduled thread to run at a time.
- At each switch point, call strategy hooks:
  - `on_switch_point(thread_id, location)`
  - `should_switch()`
  - `choose_thread(current_thread)`
- Block non-scheduled threads using condition variables or semaphores.
- Support aborting all other threads after a detected failure.
- Track thread states: runnable, blocked, parked, finished, aborted.
- Track cooperative locks: owner thread, reentrancy depth, waiters, lock acquisition attempts, unlocks, and lock-related blocking reasons.
- When a model-checked thread blocks on a Lincheck-managed lock, mark it blocked and schedule another runnable thread instead of letting the OS scheduler decide.
- Distinguish ordinary switch points from mandatory blocking switches so deadlocks and lock-order bugs are reported as such.

Use real threads first. Fibers can be evaluated later if OS thread scheduling makes deterministic replay fragile.

### Memory and Synchronization Tracking

MVP:

- Track accesses only through Lincheck wrappers.
- Treat model checking as sequentially consistent.
- Record read/write trace points with stable object IDs and location IDs.
- Track locks and condition variables from Lincheck wrappers with enough state to model lock contention, recursive depth, timed-lock timeout, blocking, and wakeup order.
- Track explicit park/unpark through `lincheck::parker` with managed parked-thread state and trace points.
- Track semaphore acquire/release through `lincheck::counting_semaphore<N>` and `lincheck::binary_semaphore` with managed permit counts, waiter queues, and timed-acquire timeout traces.
- Track latch count-down/wait through `lincheck::latch` with managed one-shot counts and waiter queues.
- Track reusable barrier arrive/wait/drop through `lincheck::barrier` with managed phase counts and waiter queues.
- Track atomic wait/notify through `lincheck::atomic<T>` and `lincheck::atomic_ref<T>` with managed waiter state and wakeup traces.
- Track atomic operations in terms of C++ operation names and memory order, but initially explore only SC behavior.

STM target support:

- Add a small `lincheck::stm` hook interface that an STM can call without depending on the full checker implementation.
- The hook API should include:
  - `tx_begin(read_only, start_clock_or_version)`
  - `tx_read(address, lock_slot, version)`
  - `tx_write(address, lock_slot)`
  - `tx_validate_begin()` / `tx_validate_end(success)`
  - `tx_lock_attempt(lock_slot)` / `tx_lock_acquired(lock_slot)` / `tx_lock_failed(lock_slot)` / `tx_lock_released(lock_slot)`
  - `tx_commit_attempt()` / `tx_commit_success(commit_clock)`
  - `tx_abort(reason)` and `tx_retry(reason, attempt)`
- The generic hook layer should attach per-thread transaction IDs and transaction depth to trace events so retries and lock events from the same attempt can be correlated.
- Trace formatting must preserve zero-valued STM metadata such as lock slot `0`, version `0`, and start/commit clock `0`; these are valid values for real STM targets and must not be treated as "missing."
- For Multiverse, start with an adapter around the ADT test harness and public transaction entry points when that provides enough signal.
- If internal transaction scheduling is needed, candidate hook points in the editable `upstream/mvcc_tm` checkout are transaction begin, commit validation, commit timestamp/unlock, abort paths, read/write barriers, and version/lock acquisition points in `multiverse/multiverse.hpp`.
- Treat these hooks as scheduling and tracing points, not as a separate STM correctness proof.
- Continue verifying only public ADT operation histories. Do not attempt opacity, serializability of live/aborted transactions, or internal STM invariant checking in this plan.

Later:

- Model `memory_order_relaxed`, release/acquire, release sequences, and read-modify-write chains.
- Port or redesign the event-structure strategy for C++ memory-model exploration.
- Consider integrating with ThreadSanitizer-style instrumentation for whole-program access tracking.

### Trace and Reporting

Port the text trace first:

- Scenario table.
- Failure type and verifier explanation.
- Thread-by-thread interleaving table.
- Clock-sorted `interleaving:` summary derived from operation invocation/response intervals.
- Switch reasons.
- Access values and operation return values.
- Operation invocation/response clocks.
- Throwing operations should still contribute operation-clock rows with the exception value, even when the exception is an unexpected checker failure.
- Verifier explanations for invalid histories, including candidate rejection reasons and sequential return mismatches.
- STM transaction events when an STM adapter is installed.
- Lock wait/unblock reasons.
- Optional state representation callback.

Current trace status:

- `CheckResult::trace_events` now retains generic runtime trace points for stress runs, model-checking schedules, obstruction-freedom failure runs, and `run_concurrent_test(...)`, including operation start/finish, switch points, thread lifecycle events, wrapper trace messages, thread IDs, unified event indexes, active public-operation context when available, and STM transaction ID/depth metadata for STM hook events and their scheduler switch points. Failure traces include a `trace events:` section alongside structured memory, STM, source-access, synchronization, dependency, interleaving, and clock sections.

Defer binary trace format, network streaming, and IDE integration.

### Instrumentation Roadmap

Phase 1 should be explicit instrumentation only. It is reliable, portable, and makes C++ data-race constraints clear.

Phase 2 can add opt-in source macros:

```cpp
#define LC_READ(x) lincheck::read(x, LC_LOCATION)
#define LC_WRITE(x, v) lincheck::write(x, v, LC_LOCATION)
#define LC_CALL(name, fn) lincheck::call(name, LC_LOCATION, fn)
#define LC_SWITCH() lincheck::switch_point(LC_LOCATION)
```

The current source macro path records stable source locations and object IDs, and `LC_CALL` traces non-void helper return values when the result can be formatted as a Lincheck value.

The current source-audit bridge adds `lincheck::audit_source_text(...)`, `lincheck::audit_source_file(...)`, and the `lincheck_source_audit` CLI. It lexically flags raw standard thread, atomic, mutex, condition-variable, wait, semaphore, barrier, latch, pthread, futex, and compiler atomic synchronization APIs that need Lincheck wrappers or adapters, with path filters, token filters, line filters, and inline suppressions. This helps keep explicitly instrumented tests honest while transparent Clang/LLVM instrumentation is still absent.

Phase 3 can investigate Clang/LLVM instrumentation:

- Clang plugin or LLVM pass inserting runtime callbacks around loads, stores, atomics, thread creation, mutex operations.
- Sanitizer-style runtime with suppression lists.
- Debug info integration for source locations.

This should remain optional because compiler instrumentation will be toolchain-specific and more expensive to maintain. A real Clang/LLVM prototype needs development packages for the active toolchain, such as `llvm-18-dev`, `libclang-18-dev`, `clang-tools-18`, and `libclang-cpp18-dev` on the current Ubuntu Clang 18 environment.

## Milestones

### Phase 0: Legal and Repository Setup

Deliverables:

- Decide whether the C++ port will copy code or reimplement from behavior.
- Add license notices and third-party dependency policy.
- Create CMake project skeleton with CI.
- Choose test framework: GoogleTest or Catch2.

Exit criteria:

- Empty library builds on Linux with Clang and GCC.
- License policy documented.

Current status:

- The implementation is treated as a behavioral reimplementation for private use; `LICENSE_POLICY.md` documents the copy/dependency policy and upstream review caveats.
- The test harness uses framework-independent executable tests for now.
- The CMake project builds the header-only library and smoke tests locally. `multiverse_smoke_tests` is conditional on the patched `upstream/mvcc_tm` checkout being present.
- GitHub Actions CI builds and tests with GCC and Clang, fetching the pinned Multiverse target and applying `third_party/mvcc_tm-lincheck-hooks.patch` before running the Multiverse smoke tests.

### Phase 1: Core Data Structures and Verifier

Deliverables:

- `Actor`, `Scenario`, `ExecutionResult`, `Value`, `Options`.
- Random parameter generators.
- Linearizability verifier with copyable sequential model.
- Unit tests for counter, stack, queue.

Exit criteria:

- Given hand-authored execution results, verifier accepts valid histories and rejects invalid histories.

### Phase 2: Stress Runner

Deliverables:

- Thread pool or per-invocation thread runner.
- Scenario execution and result collection.
- Timeout and exception handling.
- Basic failure report.

Exit criteria:

- Stress tests find known broken counter/queue examples within bounded repetitions.

Current status:

- Stress mode runs per-invocation real worker threads behind a ready gate, collects operation results, applies the same linearizability verifier, reports cooperative timeouts and unexpected exceptions, and retains failure traces.
- Tests include deterministic forced-overlap stress failures for a broken counter and a broken single-slot queue, plus timeout, validation, exception, and invalid-result report coverage.

### Phase 3: Cooperative Model Checker

Deliverables:

- Scheduler runtime.
- Interleaving tree from `ModelCheckingStrategy`.
- `lincheck::thread`, `lincheck::atomic`, `lincheck::var`, `lincheck::mutex`.
- Deterministic replay.

Exit criteria:

- Model checker deterministically finds the lost-update counter bug and prints a replayable schedule.

### Phase 4: Trace, Minimization, and Usability

Deliverables:

- Scenario minimization.
- Text trace table.
- Invalid-result verifier explanation sections.
- Interleaving summary sections for recorded operation histories.
- State representation callback.
- Better failure taxonomy.
- Fail-fast validation for invalid runner options.
- Documentation and examples.

Exit criteria:

- Reports are useful without a debugger and match the clarity of JVM Lincheck examples for small cases.

Current status:

- Text failure traces include failure summaries, verifier explanations, interleaving and operation-clock sections, state callbacks, structured trace events, structured memory/STM/source/synchronization streams, event dependencies, and operation dependency footprints. The binary trace format remains deferred.

### Phase 5: Parity Features

Deliverables:

- Non-parallel operation groups.
- Obstruction-freedom check.
- Lock/condition-variable trace details.
- Atomic compare-exchange and fetch operations.
- Golden trace tests.

Exit criteria:

- Most JVM data-structure guide examples have C++ equivalents.

### Phase 6: Clocks, Locks, and STM Hooks

Deliverables:

- `ClockSource` abstraction with strict atomic-counter and optional `rdtsc`/`rdtscp` implementations plus runtime warnings.
- Operation interval recording and verifier real-time ordering.
- Cooperative model-checker lock tracker for mutex and condition-variable blocking.
- `lincheck::stm` hook interface.
- Multiverse adapter and hook prototype for `upstream/mvcc_tm`.
- Runtime warnings when explicit atomic and fence wrappers observe non-`seq_cst` memory orders, making the current sequentially-consistent exploration caveat visible in `CheckResult::warnings`.
- Fail-fast validation for unknown memory-order enum values and C++-invalid atomic load/store/wait/CAS order combinations before wrapper calls reach `std::atomic`.

Exit criteria:

- A small ADT implemented with Multiverse transactions can be checked through public operation registration.
- Failure traces include ADT operations, transaction events with transaction IDs, lock/validation/commit points, and operation clocks.

Current status:

- `tests/multiverse_smoke_tests.cpp` includes a Multiverse-backed counter, a small fixed-capacity Multiverse-backed set, a multi-field Multiverse-backed bank/account ADT, and a shared-array ADT whose generated operations apply random integer deltas to fixed-size slot groups.
- `examples/multiverse_tiny_set_example.cpp` and `examples/multiverse_bank_example.cpp` are standalone starter applications for STM-backed ADT patterns.
- The set registers `add`, `remove`, and `contains` public ADT operations against a `std::set` sequential model with generated key domains. The bank registers bidirectional transfer and balance/total operations against a two-account sequential model, exercising transactions that update multiple `tx_field` objects in one public operation. The shared-array workload uses a lightweight contribution-total model plus final sum validation instead of an explicit sequential array model. Its intentional missing-slot variant is covered both by final validation and by a validation-free spec with a registered post `sum()` operation that fails as `invalid_results`.
- Model checking covers representative set, bank, and shared-array scenarios. Stress mode covers generated set, transfer, and shared-array apply workloads; the shared-array generated slot groups are intentionally low-contention to avoid abort/retry storms dominating smoke-test runtime. Larger shared-array variants need explicit contention budgeting because high-overlap slot choices can produce very high STM abort/retry rates and the appearance of deadlock or livelock.
- STM hook events are used only for scheduling and trace visibility; verification remains at the public ADT operation boundary.

### Phase 7: Optional Compiler Instrumentation

Deliverables:

- Prototype Clang/LLVM pass or source-level instrumentation macros.
- Source location metadata.
- Suppression and inclusion filters.

Exit criteria:

- A small unmodified C++ data structure can be analyzed without manually replacing every shared access.

Current status:

- Source-level macros provide explicit instrumentation with source locations.
- `lincheck_source_audit` provides suppression and inclusion filters for finding raw synchronization APIs that still need wrappers or instrumentation, including standard synchronization APIs and platform-specific waits; standard mutexes, recursive mutexes, timed mutexes, shared mutexes, semaphores, barriers, and latches now have Lincheck wrappers and lexical rewrites.
- `lincheck_source_rewrite` provides a preliminary lexical source-to-source bridge for source files or directories, rewriting supported standard tokens such as `std::atomic`, `std::atomic_ref`, `std::thread`, `std::jthread`, `std::mutex`, `std::recursive_mutex`, `std::timed_mutex`, `std::recursive_timed_mutex`, `std::shared_mutex`, `std::shared_timed_mutex`, `std::shared_lock`, `std::condition_variable`, `std::binary_semaphore`, `std::counting_semaphore`, `std::barrier`, `std::latch`, lock wrappers, `std::this_thread::yield`, `std::this_thread::sleep_for`, `std::this_thread::sleep_until`, and standard atomic fences to Lincheck equivalents while preserving comments, string literals, suppressions, and path/token filters. Directory mode is available for `--check` and `--in-place`; stdout and `--output=FILE` remain single-file modes.
- CTest rewrites, compiles, and runs a small raw-threading fixture as a smoke test for generated source, while still treating the rewriter as a lexical bridge rather than a whole-program parser.
- `lincheck_clang_source_audit` is an optional LibTooling-based source-audit prototype. When LLVM/Clang CMake targets are available, it AST-checks main-file declarations and direct calls for raw standard atomics, threads, mutexes, condition variables, waits, semaphores, barriers, latches, and compiler atomic builtins. It shares the lexical audit's path filters, token filters, line filters, and inline suppressions. It exits nonzero on findings and has CTest coverage for raw-sync detection, Lincheck-wrapper acceptance, suppression, and path filtering.
- A transparent Clang/LLVM pass or real LibTooling rewriter is not implemented yet.

### Phase 8: C++ Memory Model and Partial Order Reduction

Deliverables:

- Formal model for C++ atomics and fences.
- Release/acquire and relaxed exploration.
- Port or redesign event-structure consistency checking.
- Partial order reduction to control state explosion.

Exit criteria:

- The checker can explain weak-memory failures beyond sequential consistency.

Current status:

- Stress and model-checking options expose `memory_model(...)`, with `MemoryModel::sequential_consistency` as the only supported mode.
- Unsupported weak-memory modes such as `cxx_release_acquire` and `cxx_relaxed` fail fast so callers cannot accidentally treat the SC checker as a C++ weak-memory checker.
- The SC model checker skips repeated verifier calls for completed schedules whose public operation history has the same operations, return values, and real-time-order constraints as an already verified successful history. This is a conservative verifier-cache reduction, not a replacement for event-structure POR.
- `CheckResult::schedule_decisions` now retains typed `OperationContext` records for runnable threads that are inside public ADT operations at scheduler decision points. Failure traces render these as a compact `operations:` suffix, and direct in-memory decision replay validates them. Operations can be explicitly marked with `independent_operation_group(...)`; when callers opt in with `ModelCheckingOptions::operation_context_reduction()`, the observed-frontier traversal prunes alternative prefixes where the chosen and alternative runnable threads are both inside operations from the same independence group. This is a user-asserted operation-level reduction, not a full event-structure POR implementation.
- Explicit atomic, atomic-ref, and fence wrappers publish structured `MemoryEvent` records through `Runtime::memory_event(...)`, and normal model-checking, stress, and `run_concurrent_test` results preserve observed records in `CheckResult::memory_events` with stable object IDs and source-location IDs cached at record time when available. STM hooks are also retained in `CheckResult::stm_events` with stable address IDs, transaction metadata, lock/version/clock fields, validation success flags, attempts, and reasons. Source-level `LC_READ`/`LC_WRITE` instrumentation is retained in `CheckResult::source_accesses` with stable object/location IDs and captured values when representable. Mutex, condition-variable, atomic wait/notify, parker, semaphore, latch, and barrier wrappers publish structured synchronization records retained in `CheckResult::synchronization_events`, including stable object IDs and related lock IDs where applicable. Event records retain the active public ADT operation context when the runtime has one. Failure traces include `trace events:`, `memory events:`, `stm events:`, `source accesses:`, and `synchronization events:` sections when records are present; the current checker records metadata plumbing only and still executes wrapper operations under the SC scheduler.
- `CheckResult::event_dependencies` now builds a conservative dependency summary from the retained trace, memory, STM, source-access, and synchronization streams. Structured event records carry a unified `event_index`; the graph records event nodes plus same-thread and same-resource edges within each stream, then uses `event_index` to add cross-stream same-thread and same-resource edges using generic trace points, trace-level STM transaction IDs, and stable object/location/address/lock IDs. Dependency nodes inherit active public-operation context from their source event, and JSON/DOT exports include that label for offline analysis and future event-structure POR work. `CheckResult::operation_dependency_footprints` groups labeled dependency nodes by public operation and records each operation's event count, event-index range, streams, and touched resources. `CheckResult::event_dependency_analysis` validates edge endpoints, checks acyclicity, and returns a topological node order for consistent graphs. Failure traces include `event dependencies:`, `event dependency analysis:`, and `operation dependency footprints:` sections. `ModelCheckingOptions::event_dependency_reduction()` is an opt-in observed-frontier reduction that prunes alternative prefixes only when both runnable operations have retained footprints and their shared-resource sets are disjoint; transaction-local `tx#...` resources alone do not justify pruning. This is an event-structure consistency foundation and an exploratory SC reduction, not yet a weak-memory consistency checker or full event-structure POR algorithm.

## Major Risks

1. Transparent instrumentation is the hardest part.
   JVM Lincheck gets switch points from ASM bytecode rewriting. C++ has no comparable portable runtime hook. Start explicit.

2. C++ data races are undefined behavior.
   A model checker cannot safely observe arbitrary racy plain loads/stores after the compiler has optimized them. MVP tests must use Lincheck wrappers for shared state.

3. Sequential model snapshots can be expensive.
   Copying the sequential model at each DFS branch is simple but may be slow. Add undo logs later.

4. Real thread abort is unsafe in C++.
   JVM Lincheck has thread-stop escape hatches for old JDKs. C++ should use cooperative cancellation and require instrumented code to reach cancellation points.

5. Blocking APIs are broad.
   Raw `std::mutex`, raw `std::condition_variable`, raw `std::atomic::wait`, raw standard semaphores/barriers/latches, platform futexes, custom spin locks, and third-party synchronization primitives need wrappers or instrumentation.

6. Full C++ memory-model checking is a separate research project.
   Sequential consistency is a practical MVP. Explicit wrappers record and warn on non-`seq_cst` order labels, and reject C++-invalid order combinations, but release/acquire and relaxed behavior should be a later, explicitly scoped phase.

7. `rdtsc` is not always a reliable cross-thread order source.
   Raw TSC values can be inconsistent across sockets, NUMA nodes, virtualized environments, older CPUs without invariant/synchronized TSC, and aggressive power-management settings. Provide a strict atomic-sequence fallback and warn when using TSC-derived clocks.

8. STM hooks can overfit to Multiverse internals.
   Keep the public hook surface small and describe events in generic transaction terms so a later STM backend can reuse the scheduler and verifier machinery.

## Initial File/Module Mapping for the C++ Port

Suggested C++ tree:

```text
cpp-lincheck/
  CMakeLists.txt
  include/lincheck/
    lincheck.hpp
    options.hpp
    scenario.hpp
    value.hpp
    generators.hpp
    verifier.hpp
    stress.hpp
    model_checking.hpp
    runtime.hpp
    trace.hpp
    clock.hpp
    stm.hpp
    wrappers/atomic.hpp
    wrappers/thread.hpp
    wrappers/mutex.hpp
    wrappers/recursive_mutex.hpp
    wrappers/timed_mutex.hpp
    wrappers/recursive_timed_mutex.hpp
    wrappers/shared_mutex.hpp
    wrappers/shared_timed_mutex.hpp
    wrappers/shared_lock.hpp
    wrappers/condition_variable.hpp
    wrappers/barrier.hpp
    wrappers/latch.hpp
    wrappers/semaphore.hpp
    wrappers/var.hpp
  src/
    scenario.cpp
    random_execution_generator.cpp
    linearizability_verifier.cpp
    stress_strategy.cpp
    model_checking_strategy.cpp
    scheduler.cpp
    trace_reporter.cpp
    minimizer.cpp
    clock.cpp
    stm_hooks.cpp
  tests/
    verifier_tests.cpp
    stress_counter_tests.cpp
    model_counter_tests.cpp
    queue_tests.cpp
    lock_model_tests.cpp
    stm_multiverse_adapter_tests.cpp
```

The current implementation remains header-only, but the public include tree now provides forwarding headers for the planned modules so downstream code can include component paths such as `lincheck/value.hpp`, `lincheck/model_checking.hpp`, and `lincheck/wrappers/atomic.hpp` without depending on the monolithic implementation filename.

## Recommended First Implementation Order

1. Implement `Value`, `Actor`, `ExecutionScenario`, and generators.
2. Implement linearizability verifier against manually authored histories.
3. Implement stress runner.
4. Add a broken counter and broken queue example.
5. Implement scheduler and cooperative model checking for `lincheck::var`.
6. Add `lincheck::atomic` and `lincheck::mutex`.
7. Add minimization and traces.
8. Add operation clocks, lock blocking semantics, and STM hooks.
9. Add a Multiverse-backed ADT example.

The Multiverse-backed ADT examples currently live in `examples/multiverse_tiny_set_example.cpp` and `examples/multiverse_bank_example.cpp`. They are built as `multiverse_tiny_set_example` and `multiverse_bank_example` when the patched `upstream/mvcc_tm` checkout is available.

This order creates value before tackling compiler instrumentation, and it gives every later runtime feature a verifier and test harness to plug into.

## Resolved Design Decisions

- Sequential specifications use copy snapshots by default. Non-copyable sequential models can opt in with `sequential_cloner(...)`; undo logs remain a later performance optimization.
- Model checking uses real OS threads with cooperative scheduling at Lincheck switch points. Fibers and coroutines are out of scope for this port.
- The current test harness is framework-independent executable tests. A public GoogleTest, Catch2, or doctest adapter can be added later without changing the core library.
- `Value` adapts common scalar/string/optional results and supports custom result types through equality, formatting, and hashing support rather than requiring a separate registration API for every custom type.
- The implementation is a behavioral reimplementation for private use. Do not copy substantial JVM Lincheck source text without a separate license review.
- The default clock source for deterministic CI-style checks is the atomic sequence clock. `rdtsc`/`rdtscp` is available as an explicit low-overhead option with NUMA, socket, virtualization, and invariant-TSC caveats.
- Multiverse integration uses direct hook call sites in `upstream/mvcc_tm/multiverse/multiverse.hpp`, preserved as `third_party/mvcc_tm-lincheck-hooks.patch`, because ADT-level adapters alone do not expose enough internal transaction switch points.
- Current STM hook events are both trace events and cooperative scheduler switch points. Verification still happens only at the public ADT operation boundary.

## Success Definition

A credible first C++ port is not "all JVM Lincheck features in C++." It is:

- A small library that can test C++ concurrent data structures.
- A stress strategy and a deterministic cooperative model-checking strategy.
- Linearizability verification against sequential specs.
- Reproducible failing schedules for explicitly instrumented code.
- Clear docs explaining that transparent whole-program C++ instrumentation is a later optional layer.

For the STM target extension, success additionally means a Multiverse-backed ADT can be checked at the ADT boundary, with deterministic schedules driven by transaction/lock hooks and traces that include operation clocks and commit/abort events.
