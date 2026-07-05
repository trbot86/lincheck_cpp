# Using C++ Lincheck

This project is a header-only C++20 Lincheck-style test library. It can run stress tests and bounded cooperative model checks for explicitly instrumented C++ concurrent objects.

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

When Clang/LLVM development packages are installed, CMake can also build the AST audit prototype:

```bash
cmake --build build --target lincheck_clang_source_audit
./build/lincheck_clang_source_audit path/to/source.cpp -- -std=c++20 -I/path/to/lincheck/include
```

This tool checks main-file declarations and direct calls for raw standard synchronization APIs. It is also only an audit tool. It does not rewrite or instrument the program.

## Transactional Memory

Lincheck++ supports STM-backed objects through explicit STM hooks. The hooks do not replace the normal ADT specification: you still register public operations against a sequential model, and Lincheck verifies the public operation history for linearizability.

The STM extension gives the model checker visibility inside a transaction implementation. A backend can call:

- `lincheck::stm::tx_location_init(address, value)`
- `lincheck::stm::tx_location_register(address, label, type_name)`
- `lincheck::stm::tx_location_destroy(address)`
- `lincheck::stm::tx_begin(read_only, start_clock_or_version)`
- `lincheck::stm::tx_read(address, lock_slot, version)`
- `lincheck::stm::tx_write(address, lock_slot)`
- `lincheck::stm::tx_read_value(address, value, lock_slot, version)`
- `lincheck::stm::tx_write_value(address, value, lock_slot)`
- `lincheck::stm::tx_validate_begin()` and `lincheck::stm::tx_validate_end(success)`
- `lincheck::stm::tx_lock_attempt(lock_slot)`, `lincheck::stm::tx_lock_acquired(lock_slot)`, `lincheck::stm::tx_lock_failed(lock_slot)`, and `lincheck::stm::tx_lock_released(lock_slot)`
- `lincheck::stm::tx_commit_attempt()` and `lincheck::stm::tx_commit_success(commit_clock)`
- `lincheck::stm::tx_abort(reason)` and `lincheck::stm::tx_retry(reason, attempt)`
- `lincheck::stm::tx_attempt_metadata(logical_transaction_id, attempt)`

When a Lincheck runtime is active, each hook records a structured `CheckResult::stm_events` entry, emits a trace event with transaction ID/depth metadata, and creates a cooperative scheduler switch point for model checking. Hooks are cheap outside Lincheck runs, but they are explicit instrumentation; there is no compiler pass that inserts them automatically.

Use this pattern for an STM-backed ADT:

1. Implement the concurrent object with the STM's normal transaction API.
2. Register only the public ADT operations in `lincheck::test<Concurrent, Sequential>()`.
3. Provide a sequential model for those public operations.
4. Add `state_representation(...)`, `validation(...)`, or post operations when important state is not otherwise observable through returns.
5. Install STM hook calls in the backend or include a backend-specific Lincheck binding header before the STM header.
6. Run both bounded model checks for small representative scenarios and stress checks for real-thread retry behavior.

The STM hooks are scheduling and diagnosis aids. A passing model check means no public linearizability violation was found among the visible, bounded hook schedules. It does not prove opacity, live-transaction serializability, internal STM invariants, weak-memory behavior, or behavior at unhooked STM code paths.

High-contention STM workloads can abort and retry heavily. Dense key or slot overlap, high transaction sizes, high thread counts, or large iteration counts can make tests look like deadlock or livelock because transactions keep retrying. Start with small domains and low expected overlap, then raise contention deliberately.

## Multiverse STM Tests

The Multiverse integration has manual setup because the upstream checkout must be patched with hook call sites.

For a fresh checkout:

```bash
mkdir -p upstream
git clone --recursive https://gitlab.com/Coccimiglio/mvcc_tm.git upstream/mvcc_tm
git -C upstream/mvcc_tm checkout 0d1454cb7d902d6534cb79dc788f08395906bb44
git -C upstream/mvcc_tm apply ../../third_party/mvcc_tm-lincheck-hooks.patch
```

Then configure and run:

```bash
cmake -S . -B build
cmake --build build --target multiverse_smoke_tests -j2
./build/multiverse_smoke_tests
```

To bind hooks in code, include the Lincheck binding before Multiverse:

```cpp
#include <lincheck/multiverse_hooks.hpp>
#include "multiverse/multiverse.hpp"
```

The hook macros are no-ops unless the binding header is included first. Lincheck verifies public ADT operations built on top of the STM. It does not prove STM opacity or internal STM serializability.

High-contention STM workloads can abort and retry heavily. Dense shared-array slot overlap, high `k`, high thread counts, or large iteration counts can look like deadlock or livelock because the STM keeps retrying. Keep smoke-test domains small and estimate expected overlap before increasing parameters.

## Important Limits

- No transparent compiler, LLVM, or Clang instrumentation is implemented.
- No whole-program load/store instrumentation is automated.
- No fibers or coroutines are supported.
- No JVM compatibility layer is provided.
- The checker explores wrapper behavior as sequentially consistent. `memory_model(...)` rejects weak-memory modes today.
- Non-`seq_cst` atomic and fence orders are recorded and warned about, but not weak-memory explored.
- Model-checker aborts, stress timeouts, and obstruction-freedom checks are cooperative. Code must reach Lincheck callbacks or switch points.
- Raw platform waits, custom spin locks, third-party synchronization primitives, and raw standard waits outside Lincheck wrappers need wrappers, source macros, or explicit adapters.
- `rdtsc`/`rdtscp` clocks can be inconsistent across sockets, NUMA nodes, virtual machines, older CPUs without invariant synchronized TSC, and aggressive power-management settings. Use the atomic-sequence clock when strict cross-thread ordering matters.
