#include <lincheck/multiverse_hooks.hpp>

#include "multiverse/multiverse.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct MultiverseCounter {
    ns_multiverse::tx_field<int> value;

    MultiverseCounter() {
        value.data = 0;
    }

    int inc() {
        int result = 0;
        ns_multiverse::updateTx([&] {
            const int observed = value.load();
            value.store(observed + 1);
            result = observed + 1;
        });
        return result;
    }
};

struct SequentialCounter {
    int value = 0;

    int inc() {
        return ++value;
    }
};

struct WrongSequentialCounter {
    int value = 0;

    int inc() {
        value += 2;
        return value;
    }
};

struct MultiverseTinySet {
    static constexpr int empty = 0;
    static constexpr std::size_t capacity = 3;
    std::array<ns_multiverse::tx_field<int>, capacity> slots;

    MultiverseTinySet() {
        for (auto& slot : slots) {
            slot.data = empty;
        }
    }

    bool add(int key) {
        bool inserted = false;
        ns_multiverse::updateTx([&] {
            for (auto& slot : slots) {
                if (slot.load() == key) {
                    inserted = false;
                    return;
                }
            }
            for (auto& slot : slots) {
                if (slot.load() == empty) {
                    slot.store(key);
                    inserted = true;
                    return;
                }
            }
            inserted = false;
        });
        return inserted;
    }

    bool remove(int key) {
        bool removed = false;
        ns_multiverse::updateTx([&] {
            for (auto& slot : slots) {
                if (slot.load() == key) {
                    slot.store(empty);
                    removed = true;
                    return;
                }
            }
            removed = false;
        });
        return removed;
    }

    bool contains(int key) {
        bool found = false;
        ns_multiverse::readTx([&] {
            for (auto& slot : slots) {
                if (slot.load() == key) {
                    found = true;
                    return;
                }
            }
            found = false;
        });
        return found;
    }

    std::string debug_string() {
        std::array<int, capacity> values{};
        ns_multiverse::readTx([&] {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                values[i] = slots[i].load();
            }
        });

        std::ostringstream out;
        out << "slots=[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out << ",";
            out << values[i];
        }
        out << "]";
        return out.str();
    }
};

struct SequentialTinySet {
    std::set<int> keys;

    bool add(int key) {
        return keys.insert(key).second;
    }

    bool remove(int key) {
        return keys.erase(key) != 0;
    }

    bool contains(int key) const {
        return keys.contains(key);
    }

    std::string state_key() const {
        std::ostringstream out;
        for (const int key : keys) {
            out << key << ",";
        }
        return out.str();
    }
};

struct MultiverseBank {
    std::array<ns_multiverse::tx_field<int>, 2> balances;

    MultiverseBank() {
        balances[0].data = 2;
        balances[1].data = 1;
    }

    bool transfer_left_to_right(int amount) {
        return transfer(0, 1, amount);
    }

    bool transfer_right_to_left(int amount) {
        return transfer(1, 0, amount);
    }

    int total() {
        int result = 0;
        ns_multiverse::readTx([&] {
            result = balances[0].load() + balances[1].load();
        });
        return result;
    }

    int left() {
        return balance(0);
    }

    int right() {
        return balance(1);
    }

    std::string debug_string() {
        int left_value = 0;
        int right_value = 0;
        ns_multiverse::readTx([&] {
            left_value = balances[0].load();
            right_value = balances[1].load();
        });

        std::ostringstream out;
        out << "balances=[" << left_value << "," << right_value << "]";
        return out.str();
    }

private:
    bool transfer(std::size_t from, std::size_t to, int amount) {
        bool moved = false;
        ns_multiverse::updateTx([&] {
            const int from_balance = balances[from].load();
            if (from_balance < amount) {
                moved = false;
                return;
            }
            const int to_balance = balances[to].load();
            balances[from].store(from_balance - amount);
            balances[to].store(to_balance + amount);
            moved = true;
        });
        return moved;
    }

    int balance(std::size_t index) {
        int result = 0;
        ns_multiverse::readTx([&] {
            result = balances[index].load();
        });
        return result;
    }
};

struct SequentialBank {
    std::array<int, 2> balances{2, 1};

    bool transfer_left_to_right(int amount) {
        return transfer(0, 1, amount);
    }

    bool transfer_right_to_left(int amount) {
        return transfer(1, 0, amount);
    }

    int total() const {
        return balances[0] + balances[1];
    }

    int left() const {
        return balances[0];
    }

    int right() const {
        return balances[1];
    }

    std::string state_key() const {
        return std::to_string(balances[0]) + "," + std::to_string(balances[1]);
    }

private:
    bool transfer(std::size_t from, std::size_t to, int amount) {
        if (balances[from] < amount) {
            return false;
        }
        balances[from] -= amount;
        balances[to] += amount;
        return true;
    }
};

constexpr std::size_t shared_array_slot_count = 12;
constexpr std::size_t shared_array_k = 3;
// Keep generated stress selections disjoint. Dense k-of-n overlap can make
// Multiverse abort/retry rates dominate this smoke test.
constexpr std::array<std::array<std::size_t, shared_array_k>, 4> shared_array_selections{{
    {{0, 1, 2}},
    {{3, 4, 5}},
    {{6, 7, 8}},
    {{9, 10, 11}}
}};

const std::array<std::size_t, shared_array_k>& shared_array_selection(int selection_id) {
    if (selection_id < 0 ||
        static_cast<std::size_t>(selection_id) >= shared_array_selections.size()) {
        throw std::invalid_argument("shared array selection id is out of range");
    }
    return shared_array_selections[static_cast<std::size_t>(selection_id)];
}

int shared_array_contribution(int selection_id, int delta) {
    (void)shared_array_selection(selection_id);
    return static_cast<int>(shared_array_k) * delta;
}

struct SharedArrayTotalModel {
    int total = 0;

    int apply(int selection_id, int delta) {
        const int contribution = shared_array_contribution(selection_id, delta);
        total += contribution;
        return contribution;
    }

    int sum() const {
        return total;
    }

    std::string state_key() const {
        return std::to_string(total);
    }
};

enum class SharedArrayMode {
    atomic_transaction,
    missing_last_slot
};

template <SharedArrayMode Mode>
struct MultiverseSharedArrayBase {
    std::array<ns_multiverse::tx_field<int>, shared_array_slot_count> slots;
    std::atomic<int> expected_total{0};

    MultiverseSharedArrayBase() {
        for (auto& slot : slots) {
            slot.data = 0;
        }
    }

    int apply(int selection_id, int delta) {
        const auto& selected = shared_array_selection(selection_id);
        const int contribution = shared_array_contribution(selection_id, delta);

        ns_multiverse::updateTx([&] {
            const auto limit = Mode == SharedArrayMode::missing_last_slot
                ? selected.size() - 1
                : selected.size();
            for (std::size_t i = 0; i < limit; ++i) {
                const auto slot_index = selected[i];
                const int observed = slots[slot_index].load();
                slots[slot_index].store(observed + delta);
            }
        });

        record_contribution(contribution);
        return contribution;
    }

    int sum() {
        return transaction_sum();
    }

    std::string validate_sum() {
        const int actual = transaction_sum();
        const int expected = expected_total.load(std::memory_order_seq_cst);
        if (actual == expected) return {};
        return "shared array sum mismatch: actual=" + std::to_string(actual) +
            " expected=" + std::to_string(expected);
    }

    std::string debug_string() {
        std::array<int, shared_array_slot_count> values{};
        ns_multiverse::readTx([&] {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                values[i] = slots[i].load();
            }
        });

        std::ostringstream out;
        out << "slots=[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out << ",";
            out << values[i];
        }
        out << "] expected_total=" << expected_total.load(std::memory_order_seq_cst);
        return out.str();
    }

private:
    static thread_local int tls_contribution_total;

    void record_contribution(int contribution) {
        tls_contribution_total += contribution;
        expected_total.fetch_add(contribution, std::memory_order_seq_cst);
        lincheck::trace_event(
            "shared_array.contribution delta_total=" + std::to_string(contribution) +
            " tls_total=" + std::to_string(tls_contribution_total)
        );
    }

    int transaction_sum() {
        int result = 0;
        ns_multiverse::readTx([&] {
            for (auto& slot : slots) {
                result += slot.load();
            }
        });
        return result;
    }
};

template <SharedArrayMode Mode>
thread_local int MultiverseSharedArrayBase<Mode>::tls_contribution_total = 0;

using MultiverseSharedArray = MultiverseSharedArrayBase<SharedArrayMode::atomic_transaction>;
using MissingSlotMultiverseSharedArray = MultiverseSharedArrayBase<SharedArrayMode::missing_last_slot>;

void model_checker_accepts_multiverse_counter() {
    lincheck::TestSpec spec = lincheck::test<MultiverseCounter, SequentialCounter>()
        .operation("inc", &MultiverseCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(5)
        .check(spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "model checker should accept Multiverse-backed counter");
}

lincheck::TestSpec multiverse_tiny_set_spec() {
    return lincheck::test<MultiverseTinySet, SequentialTinySet>()
        .operation("add", &MultiverseTinySet::add, &SequentialTinySet::add, lincheck::values<int>({1, 2, 3}))
        .operation("remove", &MultiverseTinySet::remove, &SequentialTinySet::remove, lincheck::values<int>({1, 2, 3}))
        .operation("contains", &MultiverseTinySet::contains, &SequentialTinySet::contains, lincheck::values<int>({1, 2, 3}))
        .sequential_state([](const SequentialTinySet& model) {
            return model.state_key();
        })
        .state_representation([](MultiverseTinySet& set) {
            return set.debug_string();
        });
}

lincheck::TestSpec multiverse_bank_spec() {
    return lincheck::test<MultiverseBank, SequentialBank>()
        .operation(
            "transfer_left_to_right",
            &MultiverseBank::transfer_left_to_right,
            &SequentialBank::transfer_left_to_right,
            lincheck::values<int>({1, 2})
        )
        .operation(
            "transfer_right_to_left",
            &MultiverseBank::transfer_right_to_left,
            &SequentialBank::transfer_right_to_left,
            lincheck::values<int>({1, 2})
        )
        .operation("total", &MultiverseBank::total, &SequentialBank::total)
        .operation("left", &MultiverseBank::left, &SequentialBank::left)
        .operation("right", &MultiverseBank::right, &SequentialBank::right)
        .sequential_state([](const SequentialBank& model) {
            return model.state_key();
        })
        .state_representation([](MultiverseBank& bank) {
            return bank.debug_string();
        });
}

template <typename Concurrent>
auto multiverse_shared_array_base_spec() {
    return lincheck::test<Concurrent, SharedArrayTotalModel>()
        .operation(
            "apply",
            &Concurrent::apply,
            &SharedArrayTotalModel::apply,
            lincheck::values<int>({0, 1, 2, 3}),
            lincheck::values<int>({-4, -1, 2, 5})
        )
        .sequential_state([](const SharedArrayTotalModel& model) {
            return model.state_key();
        })
        .state_representation([](Concurrent& array) {
            return array.debug_string();
        })
        .validation([](Concurrent& array) {
            return array.validate_sum();
        });
}

template <typename Concurrent>
auto multiverse_shared_array_base_spec_without_validation() {
    return lincheck::test<Concurrent, SharedArrayTotalModel>()
        .operation(
            "apply",
            &Concurrent::apply,
            &SharedArrayTotalModel::apply,
            lincheck::values<int>({0, 1, 2, 3}),
            lincheck::values<int>({-4, -1, 2, 5})
        )
        .sequential_state([](const SharedArrayTotalModel& model) {
            return model.state_key();
        })
        .state_representation([](Concurrent& array) {
            return array.debug_string();
        });
}

template <typename Concurrent>
lincheck::TestSpec multiverse_shared_array_spec() {
    return multiverse_shared_array_base_spec<Concurrent>()
        .operation("sum", &Concurrent::sum, &SharedArrayTotalModel::sum);
}

template <typename Concurrent>
lincheck::TestSpec multiverse_shared_array_spec_without_validation() {
    return multiverse_shared_array_base_spec_without_validation<Concurrent>()
        .operation("sum", &Concurrent::sum, &SharedArrayTotalModel::sum);
}

template <typename Concurrent>
lincheck::TestSpec multiverse_shared_array_apply_only_spec() {
    return multiverse_shared_array_base_spec<Concurrent>();
}

lincheck::Actor tiny_set_actor(std::size_t operation_index, std::string name, int key) {
    lincheck::Actor actor;
    actor.operation_index = operation_index;
    actor.name = std::move(name);
    actor.arguments.push_back(lincheck::Value(key));
    return actor;
}

lincheck::Actor bank_actor(std::size_t operation_index, std::string name, std::vector<lincheck::Value> arguments = {}) {
    lincheck::Actor actor;
    actor.operation_index = operation_index;
    actor.name = std::move(name);
    actor.arguments = std::move(arguments);
    return actor;
}

void model_checker_accepts_multiverse_tiny_set_representative_scenario() {
    lincheck::TestSpec spec = multiverse_tiny_set_spec();

    lincheck::ExecutionScenario scenario;
    scenario.init = {
        tiny_set_actor(0, "add", 1)
    };
    scenario.parallel = {
        {tiny_set_actor(0, "add", 2)},
        {tiny_set_actor(1, "remove", 1)}
    };
    scenario.post = {
        tiny_set_actor(2, "contains", 1),
        tiny_set_actor(2, "contains", 2)
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(8)
        .max_context_switches_per_schedule(2)
        .check("multiverse-tiny-set", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "model checker should accept a representative Multiverse-backed tiny set scenario");
    require(result.stats.schedules_explored > 0, "Multiverse tiny-set model check should explore at least one schedule");
}

void model_checker_accepts_multiverse_bank_representative_scenario() {
    lincheck::TestSpec spec = multiverse_bank_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_left_to_right", {lincheck::Value(2)})},
        {bank_actor(1, "transfer_right_to_left", {lincheck::Value(2)})}
    };
    scenario.post = {
        bank_actor(2, "total"),
        bank_actor(3, "left"),
        bank_actor(4, "right")
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(64)
        .max_context_switches_per_schedule(0)
        .check("multiverse-bank", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "model checker should accept a multi-field Multiverse-backed bank scenario");
    require(result.stats.schedules_explored > 0, "Multiverse bank model check should explore at least one schedule");

    std::unordered_set<std::string> written_addresses;
    for (const auto& event : result.stm_events) {
        if (event.kind == "tx_write" && !event.address_id.empty()) {
            written_addresses.insert(event.address_id);
        }
    }
    require(
        written_addresses.size() >= 2,
        "Multiverse bank trace should include writes to multiple transactional fields"
    );
    require(
        std::any_of(result.event_dependencies.nodes.begin(), result.event_dependencies.nodes.end(), [](const auto& node) {
            return node.stream == "stm";
        }),
        "Multiverse bank result should retain STM event dependency nodes"
    );
}

void stress_runner_accepts_multiverse_tiny_set_with_generated_keys() {
    lincheck::TestSpec spec = multiverse_tiny_set_spec();

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(4)
        .threads(2)
        .actors_per_thread(2)
        .actors_before(1)
        .actors_after(1)
        .seed(20260704)
        .check("multiverse-tiny-set", spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "stress runner should accept a generated Multiverse-backed tiny set workload");
}

void stress_runner_accepts_multiverse_bank_with_generated_transfers() {
    lincheck::TestSpec spec = multiverse_bank_spec();

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(2)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(1)
        .seed(20260705)
        .check("multiverse-bank", spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "stress runner should accept generated Multiverse-backed bank transfers");
}

void model_checker_accepts_multiverse_shared_array_representative_scenario() {
    lincheck::TestSpec spec = multiverse_shared_array_spec<MultiverseSharedArray>();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "apply", {lincheck::Value(0), lincheck::Value(5)})},
        {bank_actor(1, "sum")}
    };
    scenario.post = {
        bank_actor(1, "sum")
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(16)
        .max_context_switches_per_schedule(0)
        .check("multiverse-shared-array", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "model checker should accept atomic Multiverse shared-array updates");
    require(result.stats.schedules_explored > 0, "Multiverse shared-array model check should explore at least one schedule");

    std::unordered_set<std::string> written_addresses;
    for (const auto& event : result.stm_events) {
        if (event.kind == "tx_write" && !event.address_id.empty()) {
            written_addresses.insert(event.address_id);
        }
    }
    require(
        written_addresses.size() >= shared_array_k,
        "Multiverse shared-array trace should include writes to k transactional fields"
    );
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.description.find("shared_array.contribution") != std::string::npos &&
                record.description.find("tls_total=") != std::string::npos;
        }),
        "Multiverse shared-array result should retain TLS contribution trace events"
    );
}

void stress_runner_accepts_multiverse_shared_array_generated_deltas() {
    lincheck::TestSpec spec = multiverse_shared_array_apply_only_spec<MultiverseSharedArray>();

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(2)
        .threads(2)
        .actors_per_thread(2)
        .actors_before(0)
        .actors_after(0)
        .seed(20260706)
        .check("multiverse-shared-array", spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "stress runner should accept generated Multiverse shared-array deltas");
}

void model_checker_rejects_missing_slot_multiverse_shared_array_by_validation() {
    lincheck::TestSpec spec = multiverse_shared_array_spec<MissingSlotMultiverseSharedArray>();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "apply", {lincheck::Value(2), lincheck::Value(5)})}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(16)
        .max_context_switches_per_schedule(0)
        .minimize_failed_scenario(false)
        .check("broken-multiverse-shared-array-missing-slot", spec, scenario);

    require(!result.success, "missing-slot shared-array update should fail final sum validation");
    require(result.failure == lincheck::FailureKind::validation_failure, "missing-slot shared-array should fail validation");
    require(result.message.find("shared array sum mismatch") != std::string::npos, "missing-slot failure should explain the sum mismatch");
    require(result.state_representation.find("expected_total=") != std::string::npos, "missing-slot failure should include shared-array state");
}

void model_checker_rejects_missing_slot_multiverse_shared_array_by_invalid_results() {
    lincheck::TestSpec spec = multiverse_shared_array_spec_without_validation<MissingSlotMultiverseSharedArray>();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "apply", {lincheck::Value(2), lincheck::Value(5)})}
    };
    scenario.post = {
        bank_actor(1, "sum")
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(16)
        .max_context_switches_per_schedule(0)
        .minimize_failed_scenario(false)
        .check("broken-multiverse-shared-array-missing-slot-invalid-results", spec, scenario);

    require(!result.success, "missing-slot shared-array update should be observable through public sum");
    require(result.failure == lincheck::FailureKind::invalid_results, "missing-slot public sum mismatch should fail linearizability");
    require(result.message == "invalid execution results", "invalid-results failure should use the linearizability failure message");
    require(!result.verifier_explanation.empty(), "invalid-results shared-array failure should include verifier explanation");
    require(result.trace.find("verifier explanation:") != std::string::npos, "invalid-results trace should include verifier explanation");
    require(result.trace.find("sum()") != std::string::npos, "invalid-results trace should include the public sum observation");
    require(result.state_representation.find("expected_total=") != std::string::npos, "invalid-results failure should include shared-array state");
}

void multiverse_failure_trace_includes_transaction_events() {
    lincheck::TestSpec spec = lincheck::test<MultiverseCounter, WrongSequentialCounter>()
        .operation("inc", &MultiverseCounter::inc, &WrongSequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(5)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "wrong sequential spec should force a Multiverse-backed failure");
    require(result.failure == lincheck::FailureKind::invalid_results, "Multiverse mismatch should be an invalid-results failure");
    require(result.trace.find("stm.tx_begin") != std::string::npos, "Multiverse failure trace should include transaction begin event");
    require(result.trace.find("stm.tx_read") != std::string::npos, "Multiverse failure trace should include transaction read event");
    require(result.trace.find("stm.tx_write") != std::string::npos, "Multiverse failure trace should include transaction write event");
    require(result.trace.find("stm.tx_validate_begin") != std::string::npos, "Multiverse failure trace should include validation begin event");
    require(result.trace.find("stm.tx_validate_end success=true") != std::string::npos, "Multiverse failure trace should include validation result event");
    require(result.trace.find("stm.tx_lock_attempt") != std::string::npos, "Multiverse failure trace should include write-lock attempt event");
    require(result.trace.find("stm.tx_lock_acquired") != std::string::npos, "Multiverse failure trace should include write-lock acquired event");
    require(result.trace.find("stm.tx_lock_released") != std::string::npos, "Multiverse failure trace should include write-lock release event");
    require(result.trace.find("stm.tx_commit_attempt") != std::string::npos, "Multiverse failure trace should include commit attempt event");
    require(result.trace.find("stm.tx_commit_success") != std::string::npos, "Multiverse failure trace should include commit event");
    require(result.trace.find("stm events:\n") != std::string::npos, "Multiverse failure trace should include structured STM event section");
    require(result.trace.find("tx_id=") != std::string::npos, "Multiverse failure trace should include transaction IDs");
    require(result.trace.find("tx_depth=") != std::string::npos, "Multiverse failure trace should include transaction depths");
    require(!result.stm_events.empty(), "Multiverse failure should retain structured STM event records");
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_commit_success" &&
                record.has_clock &&
                record.transaction_id != 0 &&
                record.transaction_depth == 1 &&
                lincheck::to_string(record).find("stm.tx_commit_success") != std::string::npos;
        }),
        "Multiverse failure should expose retained commit event metadata"
    );
    require(result.trace.find("operation clocks:") != std::string::npos, "Multiverse failure trace should include operation clocks");
}

void multiverse_concurrent_trace_includes_abort_and_retry_events() {
    MultiverseCounter counter;
    std::atomic<bool> first_holds_write_lock{false};
    std::atomic<int> contender_attempts{0};

    const auto result = lincheck::run_concurrent_test([&] {
        lincheck::thread first([&] {
            ns_multiverse::updateTx([&] {
                const int observed = counter.value.load();
                counter.value.store(observed + 1);
                first_holds_write_lock.store(true, std::memory_order_release);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (contender_attempts.load(std::memory_order_acquire) < 2 &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                }
            });
        });

        lincheck::thread contender([&] {
            while (!first_holds_write_lock.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            ns_multiverse::updateTx([&] {
                contender_attempts.fetch_add(1, std::memory_order_acq_rel);
                const int observed = counter.value.load();
                counter.value.store(observed + 1);
            });
        });

        first.join();
        contender.join();
    });

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "forced Multiverse contention should complete after retry");
    require(result.trace.find("stm.tx_abort") != std::string::npos, "Multiverse contention trace should include transaction abort event");
    require(result.trace.find("stm.tx_retry") != std::string::npos, "Multiverse contention trace should include transaction retry event");
    require(result.trace.find("reason=abortTx") != std::string::npos, "Multiverse abort event should preserve the abort reason");
    require(result.trace.find("reason=abort") != std::string::npos, "Multiverse retry event should preserve the retry reason");
    require(result.trace.find("tx_id=") != std::string::npos, "Multiverse contention trace should include transaction IDs");
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_retry" &&
                record.attempt > 0 &&
                record.reason == "abort" &&
                record.transaction_id != 0;
        }),
        "Multiverse concurrent test should retain structured retry event records"
    );
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"model_checker_accepts_multiverse_counter", model_checker_accepts_multiverse_counter},
        {"model_checker_accepts_multiverse_tiny_set_representative_scenario", model_checker_accepts_multiverse_tiny_set_representative_scenario},
        {"model_checker_accepts_multiverse_bank_representative_scenario", model_checker_accepts_multiverse_bank_representative_scenario},
        {"stress_runner_accepts_multiverse_tiny_set_with_generated_keys", stress_runner_accepts_multiverse_tiny_set_with_generated_keys},
        {"stress_runner_accepts_multiverse_bank_with_generated_transfers", stress_runner_accepts_multiverse_bank_with_generated_transfers},
        {"model_checker_accepts_multiverse_shared_array_representative_scenario", model_checker_accepts_multiverse_shared_array_representative_scenario},
        {"stress_runner_accepts_multiverse_shared_array_generated_deltas", stress_runner_accepts_multiverse_shared_array_generated_deltas},
        {"model_checker_rejects_missing_slot_multiverse_shared_array_by_validation", model_checker_rejects_missing_slot_multiverse_shared_array_by_validation},
        {"model_checker_rejects_missing_slot_multiverse_shared_array_by_invalid_results", model_checker_rejects_missing_slot_multiverse_shared_array_by_invalid_results},
        {"multiverse_failure_trace_includes_transaction_events", multiverse_failure_trace_includes_transaction_events},
        {"multiverse_concurrent_trace_includes_abort_and_retry_events", multiverse_concurrent_trace_includes_abort_and_retry_events},
    };

    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
        }
    }

    return failed == 0 ? 0 : 1;
}
