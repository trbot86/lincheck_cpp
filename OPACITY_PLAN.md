# Plan to Add STM Opacity Checking

Date: 2026-07-05

## Goal

Add an optional STM opacity checker to Lincheck++.

The current STM support uses transaction hooks as scheduler switch points and trace metadata, then verifies public ADT operation histories for linearizability. Opacity checking is different: it must verify the transaction history itself. A passing opacity check should mean that every observed transaction attempt, including aborted and live attempts, read from some consistent serial state within the explored schedule.

This plan keeps ADT linearizability and STM opacity as separate checks:

- ADT linearizability checks public operation return values, exceptions-as-results, post operations, and real-time order.
- STM opacity checks transaction-level reads, writes, commits, aborts, and live attempts.

## Non-Goals

- No C++ weak-memory exploration. The first opacity checker assumes the same sequentially consistent execution model as the current scheduler.
- No transparent compiler instrumentation.
- No proof for unhooked transactional accesses.
- No checking of external side effects performed inside aborted transactions.
- No initial support for open nesting, closed nesting, privatization safety, or memory reclamation correctness beyond the recorded transaction locations.

## Opacity Model

For each executed schedule, build a transaction history and check whether there exists a serial order satisfying:

- All committed, aborted, and live transaction attempts respect real-time order when their begin/end intervals impose one.
- Aborted and live transactions are included as observers and must read from a consistent serial state.
- A transaction reads its own earlier write to a location when present.
- Otherwise, each read observes the latest preceding committed write to that location, or the registered initial value.
- Aborted and live transaction writes do not become visible to other transactions.
- Live transactions at the end of the schedule are treated as aborted observers over their partial read/write history.

This catches inconsistent snapshots, dirty reads from aborted transactions, stale reads that cannot be serialized, and transactions that observe impossible combinations before aborting.

## Hook Surface

Keep the existing scheduling and tracing hooks:

- `lincheck::stm::tx_begin(read_only, start_clock_or_version)`
- `lincheck::stm::tx_read(address, lock_slot, version)`
- `lincheck::stm::tx_write(address, lock_slot)`
- `lincheck::stm::tx_validate_begin()`
- `lincheck::stm::tx_validate_end(success)`
- `lincheck::stm::tx_lock_attempt(lock_slot)`
- `lincheck::stm::tx_lock_acquired(lock_slot)`
- `lincheck::stm::tx_lock_failed(lock_slot)`
- `lincheck::stm::tx_lock_released(lock_slot)`
- `lincheck::stm::tx_commit_attempt()`
- `lincheck::stm::tx_commit_success(commit_clock)`
- `lincheck::stm::tx_abort(reason)`
- `lincheck::stm::tx_retry(reason, attempt)`

Introduce these value and location hooks:

```cpp
namespace lincheck::stm {

template <typename T>
void tx_location_init(const void* address, const T& value);

void tx_location_register(
    const void* address,
    std::string label = {},
    std::string type_name = {}
);

void tx_location_destroy(const void* address);

template <typename T>
void tx_read_value(
    const void* address,
    const T& value,
    std::uint64_t lock_slot = 0,
    std::uint64_t version = 0
);

template <typename T>
void tx_write_value(
    const void* address,
    const T& value,
    std::uint64_t lock_slot = 0
);

void tx_attempt_metadata(
    std::uint64_t logical_transaction_id,
    int attempt
);

} // namespace lincheck::stm
```

Hook semantics:

- `tx_location_init` records the initial value for a logical transactional location. It should be emitted before any transaction can read or write the location.
- `tx_location_register` gives the location a stable diagnostic label and optional type name. It is useful but not required for verification.
- `tx_location_destroy` lets the history builder detect address reuse and close a location lifetime. Initial implementation can reject histories that reuse a destroyed address.
- `tx_read_value` replaces `tx_read` for opacity-enabled backends. It records the observed value and still acts as a scheduler switch point.
- `tx_write_value` replaces `tx_write` for opacity-enabled backends. It records the value placed in the transaction's write set and still acts as a scheduler switch point.
- `tx_attempt_metadata` is optional metadata for STMs that distinguish logical transactions from retry attempts. Opacity can treat every retry attempt as a separate transaction, but this metadata makes traces and failures easier to read.

No separate read-own-write hook is planned. The history builder can infer read-own-write behavior when a `tx_read_value` observes a location already written by the same transaction attempt.

All value hooks should convert values through `lincheck::Value` or the same value-adapter mechanism used for operation results. If a location value cannot be represented, opacity checking should fail fast for that run with an explanatory warning instead of silently ignoring the location.

## Multiverse Hook Macros

Extend `include/lincheck/multiverse_hooks.hpp` and `third_party/mvcc_tm-lincheck-hooks.patch` with no-op defaults and Lincheck bindings for:

- `MULTIVERSE_LINCHECK_TX_LOCATION_INIT(address, value)`
- `MULTIVERSE_LINCHECK_TX_LOCATION_REGISTER(address, label, type_name)`
- `MULTIVERSE_LINCHECK_TX_LOCATION_DESTROY(address)`
- `MULTIVERSE_LINCHECK_TX_READ_VALUE(address, value, lock_slot, version)`
- `MULTIVERSE_LINCHECK_TX_WRITE_VALUE(address, value, lock_slot)`
- `MULTIVERSE_LINCHECK_TX_ATTEMPT_METADATA(logical_transaction_id, attempt)`

For Multiverse, prefer the logical `tx_field<T>` address as the transaction location identity, not the lock-table slot. The lock slot should remain metadata. The patch should hook:

- `tx_field<T>` construction or initialization paths for `TX_LOCATION_INIT`.
- `tx_field<T>::load()` after the value and version are known for `TX_READ_VALUE`.
- `tx_field<T>::store(value)` when the write-set entry is created or updated for `TX_WRITE_VALUE`.
- transaction retry paths for attempt metadata when Multiverse exposes a retry count.
- destruction only if the target has a reliable destruction path; otherwise document that address reuse is unsupported for opacity runs.

## Data Structures

Add structured opacity data alongside existing `CheckResult::stm_events`:

```cpp
enum class StmTransactionOutcome {
    committed,
    aborted,
    live
};

struct StmReadObservation {
    StableLocationId location;
    Value value;
    std::size_t event_index;
    std::optional<std::uint64_t> version;
    std::optional<std::uint64_t> lock_slot;
    bool from_own_write = false;
};

struct StmWriteObservation {
    StableLocationId location;
    Value value;
    std::size_t event_index;
    std::optional<std::uint64_t> lock_slot;
};

struct StmTransactionAttempt {
    std::uint64_t transaction_id = 0;
    std::uint64_t logical_transaction_id = 0;
    int attempt = 0;
    int thread_id = -1;
    OperationContext operation;
    StmTransactionOutcome outcome = StmTransactionOutcome::live;
    std::size_t begin_event_index = 0;
    std::optional<std::size_t> end_event_index;
    std::vector<StmReadObservation> reads;
    std::vector<StmWriteObservation> writes;
};

struct StmOpacityHistory {
    std::map<StableLocationId, Value> initial_values;
    std::vector<StmTransactionAttempt> transactions;
    EventDependencyGraph event_dependencies;
};
```

The exact names can change during implementation, but the important requirement is that opacity gets a normalized transaction-attempt history rather than re-parsing formatted trace text.

## History Builder

Build `StmOpacityHistory` from retained structured STM events after each run:

1. Group events by `transaction_id`.
2. Attach active public `OperationContext` metadata when available.
3. Record begin/end event indexes and transaction outcome.
4. Convert read/write value hooks into per-location observations.
5. Collapse multiple writes to the same location to the last write for global visibility, while retaining the full event list for diagnostics.
6. Mark transactions with a begin but no commit/abort as `live`.
7. Validate that every read location has an initial value or a prior candidate committed write.
8. Validate that destroyed and re-registered locations are not reused unless location lifetimes are modeled.

The builder should report malformed histories separately from opacity violations, for example missing `tx_begin`, duplicate commit/abort, read outside a transaction, unsupported value type, or missing initial value.

## Opacity Verifier

Implement a dedicated verifier, separate from `LinearizabilityVerifier`.

Initial algorithm:

1. Split transactions into committed mutators and aborted/live observers.
2. Build real-time edges from transaction begin/end event indexes and operation clocks when available.
3. Backtrack over committed transaction serial orders that respect real-time edges.
4. Maintain a serial state map from location to `Value`.
5. For a committed transaction, verify each read against its own prior writes or the current serial state, then apply its final write-set to the serial state.
6. For each aborted/live observer, search for an insertion point among committed transactions that respects real-time constraints and makes all of its reads consistent. Observer writes affect only its own subsequent reads.
7. Memoize failed states by remaining committed set, serial state hash, and satisfied observer set.

This brute-force verifier is acceptable for the same small bounded histories where model checking is useful. Later optimizations can derive read-from candidates and solve an ordering-constraint graph before backtracking.

## API Integration

Add opt-in runner configuration:

```cpp
auto result = lincheck::ModelCheckingOptions()
    .check_opacity()
    .check(spec);
```

Also support stress runs:

```cpp
auto result = lincheck::StressOptions()
    .check_opacity()
    .check(spec);
```

Add result fields:

- `CheckResult::opacity_history`
- `CheckResult::opacity_explanation`
- `CheckResult::opacity_result`

Add a failure kind:

- `FailureKind::opacity_violation`

When both ADT linearizability and opacity fail, prefer reporting both explanations in the trace. The primary failure kind can be controlled by option order later; the first implementation should prioritize `opacity_violation` when opacity checking is explicitly enabled, because opacity bugs can be invisible at the public ADT boundary for small scenarios.

## Failure Reporting

Failure traces should include:

- `opacity violation:` summary.
- rejected transaction ID, logical transaction ID, attempt, thread, and public operation context.
- rejected read location/value.
- candidate serial states considered.
- committed transactions that block serialization.
- transaction history section with begin/end outcome, reads, writes, and event indexes.
- compact read-from explanation when available.

Export helpers should mirror the existing dependency graph helpers:

- `format_opacity_history_json(...)`
- `format_opacity_history_dot(...)`

## Tests

Unit tests over hand-built histories:

- committed write then committed read passes.
- read-only transaction over two locations sees a consistent snapshot.
- read-own-write passes.
- aborted transaction with consistent reads passes.
- live transaction with consistent partial reads passes.
- dirty read from an aborted writer fails.
- read-only transaction that observes `x=1, y=0` after one committed transaction wrote `x=1, y=1` fails.
- committed transaction that reads a stale value forbidden by real-time order fails.
- missing initial value is reported as malformed history, not as opacity violation.

Runtime tests with a small toy STM:

- correct deferred-update STM passes opacity and ADT linearizability.
- broken dirty-read STM fails opacity even when public ADT output happens to look linearizable.
- broken read-only snapshot STM fails opacity.
- retrying transaction histories keep attempts separate.

Multiverse tests:

- existing tiny-set and bank examples pass with opacity enabled under small scenarios.
- shared-array smoke remains low contention and passes opacity.
- value hooks appear in `stm events:` and opacity history sections.
- a deliberately broken local test STM, not Multiverse itself, should provide most negative coverage so the upstream patch is not intentionally corrupted.

## Implementation Phases

### Phase 1: Value Hook Plumbing

- Add value fields to `stm::Event` and `StmEventRecord`.
- Add `tx_location_init`, `tx_location_register`, `tx_location_destroy`, `tx_read_value`, `tx_write_value`, and `tx_attempt_metadata`.
- Keep old `tx_read` and `tx_write` as metadata-only scheduling hooks.
- Preserve existing tests and traces when opacity is disabled.

### Phase 2: History Builder

- Build `StmOpacityHistory` from structured events.
- Add malformed-history diagnostics.
- Add JSON/text formatting for transaction histories.
- Add hand-authored history-builder tests.

### Phase 3: Opacity Verifier

- Implement the brute-force opacity verifier.
- Add hand-authored opacity verifier tests.
- Add clear explanations for rejected reads and impossible observer placements.

### Phase 4: Runner Integration

- Add `check_opacity()` to model-checking and stress options.
- Run opacity verification after each schedule or stress invocation when enabled.
- Add `FailureKind::opacity_violation`.
- Include opacity sections in failure traces.

### Phase 5: Toy STM Coverage

- Add a small test-only STM with value hooks.
- Add passing and intentionally broken variants.
- Confirm opacity can fail independently from public ADT linearizability.

### Phase 6: Multiverse Value Hooks

- Extend the Multiverse hook patch and binding header with value/location macros.
- Hook `tx_field<T>` reads and writes with logical location identity and values.
- Add initial-value capture for Multiverse test objects.
- Enable opacity checks for small Multiverse smoke scenarios.

### Phase 7: Documentation and Limits

- Update `README.md` and `USING_CPP_LINCHECK.md`.
- Document hook requirements and unsupported cases.
- Document that opacity checking is bounded by visible hooks and scheduler limits.

## Acceptance Criteria

- Existing linearizability, stress, source-audit, and Multiverse tests still pass with opacity disabled.
- Hand-authored opacity histories cover committed, aborted, and live transactions.
- Broken toy STM variants fail with `FailureKind::opacity_violation`.
- Multiverse tiny-set and bank smoke scenarios pass opacity checking with value hooks.
- Failure traces show enough transaction/read/write detail to diagnose the opacity violation without inspecting raw event vectors.
- Missing hooks or unsupported values produce explicit malformed-history diagnostics, not silent false passes.

## Risks

- Hook placement in an STM backend is easy to get subtly wrong. A read hook must report the value actually returned to transaction code, and a write hook must report the value placed in that transaction's write set.
- Initial values are essential. If constructors run outside a Lincheck runtime, the checker may need either construction-time runtime capture or a `TestSpec` callback to enumerate initial STM locations.
- Address reuse can make histories unsound unless location lifetimes are recorded.
- The verifier can become expensive as transaction count and location count grow. Keep first tests small and add pruning only after correctness is established.
- Some STMs expose snapshot clocks or versions that are useful diagnostics but should not be trusted as the proof by themselves; the verifier should prove consistency from values and ordering constraints.
