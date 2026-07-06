#include <lincheck/multiverse_hooks.hpp>

#include "multiverse/multiverse.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
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

struct MultiverseContendedCounter {
    ns_multiverse::tx_field<int> value;
    std::atomic<bool> holder_has_write{false};
    std::atomic<int> contender_attempts{0};

    MultiverseContendedCounter() {
        value.data = 0;
    }

    int hold_inc() {
        int result = 0;
        ns_multiverse::updateTx([&] {
            const int observed = value.load();
            value.store(observed + 1);
            result = observed + 1;
            holder_has_write.store(true, std::memory_order_release);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (contender_attempts.load(std::memory_order_acquire) < 1 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        });
        return result;
    }

    int contend_inc() {
        int result = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!holder_has_write.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }

        ns_multiverse::updateTx([&] {
            contender_attempts.fetch_add(1, std::memory_order_acq_rel);
            const int observed = value.load();
            value.store(observed + 1);
            result = observed + 1;
        });
        return result;
    }

    int snapshot_value() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!holder_has_write.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }

        int result = 0;
        ns_multiverse::gMultiverse.transaction([&] {
            lincheck::trace_event("multiverse.contended_counter.direct_snapshot_value");
            result = value.load();
        }, ns_multiverse::TX_IS_SNAPSHOT);
        return result;
    }

    std::string debug_string() {
        int observed = 0;
        ns_multiverse::readTx([&] {
            observed = value.load();
        });
        return "value=" + std::to_string(observed) +
            " contender_attempts=" + std::to_string(contender_attempts.load(std::memory_order_seq_cst));
    }
};

struct SequentialContendedCounter {
    int value = 0;

    int hold_inc() {
        return ++value;
    }

    int contend_inc() {
        return ++value;
    }

    int snapshot_value() const {
        return value;
    }

    std::string state_key() const {
        return std::to_string(value);
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

    int snapshot_total() {
        int result = 0;
        ns_multiverse::gMultiverse.transaction([&] {
            lincheck::trace_event("multiverse.bank.direct_snapshot_total");
            result = balances[0].load() + balances[1].load();
        }, ns_multiverse::TX_IS_SNAPSHOT);
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

    int snapshot_total() const {
        return total();
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
// Keep smoke-test selections disjoint. Dense k-of-n overlap can make
// Multiverse abort/retry rates dominate these tests.
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

constexpr int snapshot_transfer_initial_value = 1000;

template <std::size_t SlotCount, int MaxUpdaterTransactions>
struct MultiverseSnapshotTransferArray {
    std::vector<ns_multiverse::tx_field<int>> slots;
    std::atomic<bool> reader_started{false};
    std::atomic<bool> reader_done{false};
    std::atomic<int> updates_completed{0};

    MultiverseSnapshotTransferArray() : slots(SlotCount) {
        for (auto& slot : slots) {
            slot.data = snapshot_transfer_initial_value;
        }
    }

    int transfer_once() {
        transfer_iteration(0);
        return 0;
    }

    int update_until_snapshot_done() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (!reader_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }

        int completed = 0;
        do {
            transfer_iteration(completed);
            ++completed;
        } while (
            !reader_done.load(std::memory_order_acquire) &&
            completed < MaxUpdaterTransactions
        );
        updates_completed.store(completed, std::memory_order_release);
        return 0;
    }

    int snapshot_sum() {
        reader_started.store(true, std::memory_order_release);
        int result = 0;
        ns_multiverse::gMultiverse.transaction([&] {
            lincheck::trace_event(
                "multiverse.snapshot_transfer_array.direct_snapshot_sum slots=" +
                std::to_string(SlotCount)
            );
            for (auto& slot : slots) {
                result += slot.load();
            }
        }, ns_multiverse::TX_IS_SNAPSHOT);
        reader_done.store(true, std::memory_order_release);
        return result;
    }

    std::string validate_total_and_activity(bool require_updates) const {
        const int actual = raw_total();
        const int expected = expected_total();
        if (actual != expected) {
            return "snapshot transfer array total mismatch: actual=" + std::to_string(actual) +
                " expected=" + std::to_string(expected);
        }
        if (require_updates && updates_completed.load(std::memory_order_acquire) <= 0) {
            return "snapshot transfer array updater did not complete any transactions";
        }
        return {};
    }

    std::string debug_string() const {
        return "slots=" + std::to_string(SlotCount) +
            " total=" + std::to_string(raw_total()) +
            " expected=" + std::to_string(expected_total()) +
            " updates_completed=" + std::to_string(updates_completed.load(std::memory_order_seq_cst));
    }

    static constexpr int expected_total() {
        return static_cast<int>(SlotCount) * snapshot_transfer_initial_value;
    }

private:
    void transfer_iteration(int iteration) {
        const auto from = static_cast<std::size_t>((iteration * 17) % static_cast<int>(SlotCount));
        const auto to = (from + SlotCount / 2 + 1) % SlotCount;
        const int delta = (iteration % 7) + 1;

        ns_multiverse::updateTx([&] {
            const int from_value = slots[from].load();
            const int to_value = slots[to].load();
            slots[from].store(from_value - delta);
            slots[to].store(to_value + delta);
        });
    }

    int raw_total() const {
        int total = 0;
        for (const auto& slot : slots) {
            total += slot.data;
        }
        return total;
    }
};

template <std::size_t SlotCount>
struct SnapshotTransferArrayModel {
    int transfer_once() { return 0; }
    int update_until_snapshot_done() { return 0; }
    int snapshot_sum() const {
        return static_cast<int>(SlotCount) * snapshot_transfer_initial_value;
    }

    std::string state_key() const {
        return std::to_string(snapshot_sum());
    }
};

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

lincheck::TestSpec multiverse_contended_counter_spec() {
    return lincheck::test<MultiverseContendedCounter, SequentialContendedCounter>()
        .operation("hold_inc", &MultiverseContendedCounter::hold_inc, &SequentialContendedCounter::hold_inc)
        .operation("contend_inc", &MultiverseContendedCounter::contend_inc, &SequentialContendedCounter::contend_inc)
        .sequential_state([](const SequentialContendedCounter& model) {
            return model.state_key();
        })
        .state_representation([](MultiverseContendedCounter& counter) {
            return counter.debug_string();
        });
}

lincheck::TestSpec multiverse_contended_counter_snapshot_spec() {
    return lincheck::test<MultiverseContendedCounter, SequentialContendedCounter>()
        .operation("hold_inc", &MultiverseContendedCounter::hold_inc, &SequentialContendedCounter::hold_inc)
        .operation("contend_inc", &MultiverseContendedCounter::contend_inc, &SequentialContendedCounter::contend_inc)
        .operation("snapshot_value", &MultiverseContendedCounter::snapshot_value, &SequentialContendedCounter::snapshot_value)
        .sequential_state([](const SequentialContendedCounter& model) {
            return model.state_key();
        })
        .state_representation([](MultiverseContendedCounter& counter) {
            return counter.debug_string();
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

lincheck::TestSpec multiverse_bank_snapshot_spec() {
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
        .operation("snapshot_total", &MultiverseBank::snapshot_total, &SequentialBank::snapshot_total)
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

bool has_prefix(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool is_multiverse_handle_location(const lincheck::StmOpacityLocation& location) {
    return location.identity_kind == "object_lifetime_field" &&
        !location.location_handle_id.empty() &&
        has_prefix(location.object_lifetime_id, "backend#multiverse.tx_field#") &&
        location.has_object_lifetime_generation &&
        location.field_id == "self";
}

void require_multiverse_handle_metadata(
    const lincheck::CheckResult& result,
    const std::string& context,
    std::size_t min_locations = 1
) {
    require(result.opacity_checked, context + " should retain opacity result fields");

    std::size_t handle_location_count = 0;
    for (const auto& [location_id, location] : result.opacity_history.locations) {
        (void)location_id;
        if (is_multiverse_handle_location(location)) {
            ++handle_location_count;
        }
    }
    require(
        handle_location_count >= min_locations,
        context + " should retain Multiverse tx_field handle-based opacity locations"
    );

    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return (record.kind == "tx_read" || record.kind == "tx_write") &&
                !record.location_handle_id.empty() &&
                has_prefix(record.object_lifetime_id, "backend#multiverse.tx_field#") &&
                record.has_object_lifetime_generation &&
                record.field_id == "self";
        }),
        context + " should emit value-access events with Multiverse handle metadata"
    );

    require(
        std::any_of(result.event_dependencies.nodes.begin(), result.event_dependencies.nodes.end(), [](const auto& node) {
            return node.stream == "stm" &&
                has_prefix(node.resource_id, "backend#multiverse.tx_field#");
        }),
        context + " should use Multiverse location handles in dependency resources"
    );
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

void stress_runner_accepts_multiverse_shared_array_disjoint_deltas() {
    lincheck::TestSpec spec = multiverse_shared_array_apply_only_spec<MultiverseSharedArray>();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {
            bank_actor(0, "apply", {lincheck::Value(0), lincheck::Value(2)})
        },
        {
            bank_actor(0, "apply", {lincheck::Value(2), lincheck::Value(2)})
        }
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .seed(20260706)
        .check("multiverse-shared-array", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "stress runner should accept disjoint Multiverse shared-array deltas");
}

void model_checker_accepts_multiverse_snapshot_transfer_array_with_opacity() {
    using Array = MultiverseSnapshotTransferArray<12, 4>;
    using Model = SnapshotTransferArrayModel<12>;
    lincheck::TestSpec spec = lincheck::test<Array, Model>()
        .operation("transfer_once", &Array::transfer_once, &Model::transfer_once)
        .operation("snapshot_sum", &Array::snapshot_sum, &Model::snapshot_sum)
        .sequential_state([](const Model& model) {
            return model.state_key();
        })
        .state_representation([](Array& array) {
            return array.debug_string();
        })
        .validation([](Array& array) {
            return array.validate_total_and_activity(false);
        });

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_once")},
        {bank_actor(1, "snapshot_sum")}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(48)
        .max_context_switches_per_schedule(0)
        .minimize_failed_scenario(false)
        .check_opacity()
        .opacity_max_committed_orders(10000)
        .check("multiverse-snapshot-transfer-array-opacity", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse snapshot transfer array should pass bounded opacity model checking");
    require_multiverse_handle_metadata(result, "Multiverse snapshot transfer array opacity smoke", 12);
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.description.find("multiverse.snapshot_transfer_array.direct_snapshot_sum") != std::string::npos;
        }),
        "snapshot transfer array model check should execute the direct snapshot operation"
    );
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_begin" && record.has_read_only && record.read_only;
        }) >= 1,
        "snapshot transfer array model check should retain read-only snapshot transaction begins"
    );
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" && record.has_value;
        }) >= 12,
        "snapshot transfer array model check should retain whole-array value reads"
    );
}

void stress_runner_reports_bounded_multiverse_snapshot_transfer_array_violation() {
    static constexpr std::size_t slot_count = 4096;
    using Array = MultiverseSnapshotTransferArray<slot_count, 64>;
    using Model = SnapshotTransferArrayModel<slot_count>;
    lincheck::TestSpec spec = lincheck::test<Array, Model>()
        .operation("update_until_snapshot_done", &Array::update_until_snapshot_done, &Model::update_until_snapshot_done)
        .operation("snapshot_sum", &Array::snapshot_sum, &Model::snapshot_sum)
        .sequential_state([](const Model& model) {
            return model.state_key();
        })
        .state_representation([](Array& array) {
            return array.debug_string();
        })
        .validation([](Array& array) {
            return array.validate_total_and_activity(true);
        });

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "update_until_snapshot_done")},
        {bank_actor(1, "snapshot_sum")}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(5000))
        .seed(20260706)
        .check("broken-multiverse-bounded-snapshot-transfer-array-stress", spec, scenario);

    require(!result.success, "bounded Multiverse snapshot transfer array should expose inconsistent snapshots");
    require(
        result.failure == lincheck::FailureKind::invalid_results,
        "inconsistent snapshot transfer array should fail as invalid public results"
    );
    require(
        !result.verifier_explanation.empty(),
        "inconsistent snapshot transfer array should include a verifier explanation"
    );
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.description.find("multiverse.snapshot_transfer_array.direct_snapshot_sum slots=4096") != std::string::npos;
        }),
        "bounded snapshot transfer array stress should execute the whole-array direct snapshot operation"
    );
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" && record.has_value;
        }) >= static_cast<std::ptrdiff_t>(slot_count),
        "bounded snapshot transfer array stress should retain whole-array value reads"
    );
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_write" && record.has_value;
        }),
        "bounded snapshot transfer array stress should retain updater writes"
    );
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

void multiverse_tx_field_handle_identity_tracks_reused_lifetimes() {
    using TxField = ns_multiverse::tx_field<int>;
    const auto result = lincheck::run_concurrent_test([] {
        alignas(TxField) unsigned char storage[sizeof(TxField)];

        auto* first = new (storage) TxField(0);
        ns_multiverse::readTx([&] {
            require(first->load() == 0, "first placement-constructed tx_field should read its initial value");
        });
        first->~TxField();

        auto* second = new (storage) TxField(7);
        ns_multiverse::readTx([&] {
            require(second->load() == 7, "reused placement-constructed tx_field should read its new initial value");
        });
        second->~TxField();
    });

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse tx_field placement reuse smoke should complete");

    const auto history = lincheck::build_stm_opacity_history(result.stm_events);
    require(history.success(), "Multiverse tx_field placement reuse should build an opacity history");
    require(history.history.locations.size() >= 2, "Multiverse tx_field placement reuse should retain both lifetimes");

    std::unordered_set<std::string> address_ids;
    std::unordered_set<std::string> object_lifetime_ids;
    std::unordered_set<std::size_t> generations;
    std::size_t handle_location_count = 0;
    for (const auto& [location_id, location] : history.history.locations) {
        (void)location_id;
        if (!is_multiverse_handle_location(location)) continue;
        ++handle_location_count;
        address_ids.insert(location.address_id);
        object_lifetime_ids.insert(location.object_lifetime_id);
        generations.insert(location.object_lifetime_generation);
    }

    require(handle_location_count >= 2, "Multiverse placement reuse should retain handle locations for both lifetimes");
    require(address_ids.size() == 1, "Multiverse placement reuse should exercise same-address tx_field reuse");
    require(object_lifetime_ids.size() >= 2, "Multiverse placement reuse should assign distinct lifetime handles");
    require(generations.contains(0) && generations.contains(1), "Multiverse placement reuse should increment lifetime generations");

    const auto verification = lincheck::verify_stm_opacity(history.history);
    if (!verification.success()) {
        std::cerr << verification.explanation << "\n";
    }
    require(verification.success(), "Multiverse tx_field placement reuse opacity history should verify");
}

void model_checker_accepts_multiverse_counter_with_opacity() {
    lincheck::TestSpec spec = lincheck::test<MultiverseCounter, SequentialCounter>()
        .operation("inc", &MultiverseCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(12)
        .minimize_failed_scenario(false)
        .check_opacity()
        .check("multiverse-counter-opacity", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse counter should pass opacity smoke test");
    require_multiverse_handle_metadata(result, "Multiverse counter opacity smoke");
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" && record.has_value && lincheck::value_cast<int>(record.value) == 0;
        }),
        "Multiverse opacity smoke should retain value-bearing read events"
    );
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_write" && record.has_value && lincheck::value_cast<int>(record.value) == 1;
        }),
        "Multiverse opacity smoke should retain value-bearing write events"
    );
}

void stress_runner_accepts_multiverse_counter_with_opacity() {
    lincheck::TestSpec spec = lincheck::test<MultiverseCounter, SequentialCounter>()
        .operation("inc", &MultiverseCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .check_opacity()
        .check("multiverse-counter-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse counter should pass stress opacity smoke test");
    require_multiverse_handle_metadata(result, "Multiverse stress counter opacity smoke");
}

void stress_runner_accepts_multiverse_contended_counter_with_opacity() {
    lincheck::TestSpec spec = multiverse_contended_counter_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "hold_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "contend_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(2000))
        .check_opacity()
        .opacity_max_committed_orders(10000)
        .check("multiverse-contended-counter-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse contended counter should pass bounded opacity stress smoke test");
    require_multiverse_handle_metadata(result, "Multiverse contended counter opacity smoke");
    require(
        result.opacity_result.observer_transaction_count > 0,
        "Multiverse contended opacity smoke should include aborted retry observers"
    );
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_retry" && record.attempt > 0;
        }),
        "Multiverse contended opacity smoke should retain structured retry events"
    );
}

void stress_runner_accepts_multiverse_high_contention_counter_with_opacity() {
    lincheck::TestSpec spec = multiverse_contended_counter_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "hold_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "contend_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "contend_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(3000))
        .check_opacity()
        .opacity_max_committed_orders(20000)
        .check("multiverse-high-contention-counter-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse high-contention counter should pass bounded opacity stress test");
    require_multiverse_handle_metadata(result, "Multiverse high-contention counter opacity smoke");
    const auto retry_count = std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
        return record.kind == "tx_retry" && record.attempt > 0;
    });
    require(
        retry_count >= 1,
        "Multiverse high-contention opacity test should retain at least one retry event"
    );
    require(
        result.opacity_result.observer_transaction_count >= 1,
        "Multiverse high-contention opacity test should include aborted retry observers"
    );
}

void stress_runner_accepts_multiverse_snapshot_during_retry_contention_with_opacity() {
    lincheck::TestSpec spec = multiverse_contended_counter_snapshot_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "hold_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "contend_inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 2, .name = "snapshot_value", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(3000))
        .check_opacity()
        .opacity_max_committed_orders(30000)
        .check("multiverse-snapshot-retry-contention-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse snapshot read should pass during bounded retry contention");
    require_multiverse_handle_metadata(result, "Multiverse snapshot retry-contention opacity smoke");
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.description.find("multiverse.contended_counter.direct_snapshot_value") != std::string::npos;
        }),
        "Multiverse snapshot retry-contention smoke should execute the direct snapshot operation"
    );
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_begin" && record.has_read_only && record.read_only;
        }),
        "Multiverse snapshot retry-contention smoke should retain a read-only transaction begin"
    );
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_retry" && record.attempt > 0;
        }),
        "Multiverse snapshot retry-contention smoke should retain updater retry events"
    );
}

void model_checker_accepts_multiverse_tiny_set_with_opacity() {
    lincheck::TestSpec spec = multiverse_tiny_set_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {tiny_set_actor(0, "add", 1)}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(24)
        .minimize_failed_scenario(false)
        .check_opacity()
        .check("multiverse-tiny-set-opacity", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse tiny-set should pass opacity smoke test");
    require_multiverse_handle_metadata(result, "Multiverse tiny-set opacity smoke");
    require(
        result.opacity_result.committed_transaction_count > 0,
        "Multiverse tiny-set opacity smoke should verify at least one committed transaction"
    );
}

void model_checker_accepts_multiverse_bank_with_opacity() {
    lincheck::TestSpec spec = multiverse_bank_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_left_to_right", {lincheck::Value(1)})}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(32)
        .minimize_failed_scenario(false)
        .check_opacity()
        .check("multiverse-bank-opacity", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse bank should pass opacity smoke test");
    require_multiverse_handle_metadata(result, "Multiverse bank opacity smoke", 2);
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_write" && record.has_value;
        }),
        "Multiverse bank opacity smoke should retain value-bearing write events"
    );
}

void stress_runner_accepts_multiverse_bank_read_only_snapshot_with_opacity() {
    lincheck::TestSpec spec = multiverse_bank_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_left_to_right", {lincheck::Value(1)})},
        {bank_actor(2, "total")}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(1000))
        .check_opacity()
        .opacity_max_committed_orders(10000)
        .check("multiverse-bank-read-only-snapshot-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse bank read-only snapshot should pass opacity smoke test");
    require_multiverse_handle_metadata(result, "Multiverse bank read-only opacity smoke", 2);
    require(
        result.opacity_result.committed_transaction_count >= 2,
        "Multiverse bank read-only opacity smoke should verify transfer and snapshot transactions"
    );
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" && record.has_value;
        }) >= 2,
        "Multiverse bank read-only opacity smoke should retain value-bearing snapshot reads"
    );
}

void stress_runner_accepts_multiverse_bank_read_heavy_snapshot_with_opacity() {
    lincheck::TestSpec spec = multiverse_bank_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_left_to_right", {lincheck::Value(1)})},
        {bank_actor(2, "total")},
        {bank_actor(2, "total")},
        {bank_actor(3, "left")},
        {bank_actor(4, "right")}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(2000))
        .check_opacity()
        .opacity_max_committed_orders(20000)
        .check("multiverse-bank-read-heavy-snapshot-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse bank read-heavy snapshot should pass bounded opacity stress test");
    require_multiverse_handle_metadata(result, "Multiverse bank read-heavy opacity smoke", 2);
    require(
        result.opacity_result.committed_transaction_count >= 5,
        "Multiverse bank read-heavy opacity smoke should verify transfer and read-only transactions"
    );
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" && record.has_value;
        }) >= 8,
        "Multiverse bank read-heavy opacity smoke should retain multiple value-bearing snapshot reads"
    );
}

void model_checker_accepts_multiverse_direct_snapshot_bank_with_opacity() {
    lincheck::TestSpec spec = multiverse_bank_snapshot_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_left_to_right", {lincheck::Value(1)})},
        {bank_actor(3, "snapshot_total")}
    };
    scenario.post = {
        bank_actor(2, "total")
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(24)
        .max_context_switches_per_schedule(0)
        .minimize_failed_scenario(false)
        .check_opacity()
        .opacity_max_committed_orders(10000)
        .check("multiverse-bank-direct-snapshot-opacity", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse direct snapshot bank scenario should pass opacity model checking");
    require_multiverse_handle_metadata(result, "Multiverse direct snapshot bank opacity smoke", 2);
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.description.find("multiverse.bank.direct_snapshot_total") != std::string::npos;
        }),
        "Multiverse direct snapshot bank smoke should execute the direct snapshot operation"
    );
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_begin" && record.has_read_only && record.read_only;
        }) >= 1,
        "Multiverse direct snapshot bank smoke should retain read-only transaction begins"
    );
}

void stress_runner_exercises_multiverse_direct_snapshot_bank_with_updaters() {
    lincheck::TestSpec spec = multiverse_bank_snapshot_spec();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "transfer_left_to_right", {lincheck::Value(1)})},
        {bank_actor(1, "transfer_right_to_left", {lincheck::Value(1)})},
        {bank_actor(3, "snapshot_total")},
        {bank_actor(3, "snapshot_total")},
        {bank_actor(2, "total")}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(3000))
        .check_opacity()
        .opacity_max_committed_orders(40000)
        .check("multiverse-bank-direct-snapshot-updaters-opacity-stress", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    if (!result.success) {
        const bool expected_diagnostic_failure =
            result.failure == lincheck::FailureKind::invalid_results ||
            result.failure == lincheck::FailureKind::opacity_violation;
        require(
            expected_diagnostic_failure,
            std::string("Multiverse direct snapshot bank updater mix should pass or report a public/opacity violation, not ") +
                lincheck::failure_kind_name(result.failure)
        );
    }
    require_multiverse_handle_metadata(result, "Multiverse direct snapshot bank updater opacity smoke", 2);
    const auto direct_snapshot_count = std::count_if(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
        return record.description.find("multiverse.bank.direct_snapshot_total") != std::string::npos;
    });
    require(
        direct_snapshot_count >= 1,
        "Multiverse direct snapshot bank updater smoke should execute a direct snapshot"
    );
    if (result.success) {
        require(
            result.opacity_result.committed_transaction_count >= 5,
            "Multiverse direct snapshot bank updater smoke should verify updater and read-only transactions"
        );
    }
    require(
        std::count_if(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" && record.has_value;
        }) >= 2,
        "Multiverse direct snapshot bank updater smoke should retain multi-location snapshot reads"
    );
}

void model_checker_accepts_multiverse_shared_array_with_opacity() {
    lincheck::TestSpec spec = multiverse_shared_array_apply_only_spec<MultiverseSharedArray>();

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {bank_actor(0, "apply", {lincheck::Value(0), lincheck::Value(2)})}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .max_schedule_length(48)
        .minimize_failed_scenario(false)
        .check_opacity()
        .check("multiverse-shared-array-opacity", spec, scenario);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "Multiverse shared-array should pass opacity smoke test");
    require_multiverse_handle_metadata(result, "Multiverse shared-array opacity smoke", shared_array_k);
    require(
        result.opacity_history.locations.size() >= shared_array_k,
        "Multiverse shared-array opacity smoke should retain initialized transactional locations"
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
        {"stress_runner_accepts_multiverse_shared_array_disjoint_deltas", stress_runner_accepts_multiverse_shared_array_disjoint_deltas},
        {"model_checker_accepts_multiverse_snapshot_transfer_array_with_opacity", model_checker_accepts_multiverse_snapshot_transfer_array_with_opacity},
        {
            "stress_runner_reports_bounded_multiverse_snapshot_transfer_array_violation",
            stress_runner_reports_bounded_multiverse_snapshot_transfer_array_violation
        },
        {"model_checker_rejects_missing_slot_multiverse_shared_array_by_validation", model_checker_rejects_missing_slot_multiverse_shared_array_by_validation},
        {"model_checker_rejects_missing_slot_multiverse_shared_array_by_invalid_results", model_checker_rejects_missing_slot_multiverse_shared_array_by_invalid_results},
        {"multiverse_failure_trace_includes_transaction_events", multiverse_failure_trace_includes_transaction_events},
        {"multiverse_concurrent_trace_includes_abort_and_retry_events", multiverse_concurrent_trace_includes_abort_and_retry_events},
        {"multiverse_tx_field_handle_identity_tracks_reused_lifetimes", multiverse_tx_field_handle_identity_tracks_reused_lifetimes},
        {"model_checker_accepts_multiverse_counter_with_opacity", model_checker_accepts_multiverse_counter_with_opacity},
        {"stress_runner_accepts_multiverse_counter_with_opacity", stress_runner_accepts_multiverse_counter_with_opacity},
        {"stress_runner_accepts_multiverse_contended_counter_with_opacity", stress_runner_accepts_multiverse_contended_counter_with_opacity},
        {"stress_runner_accepts_multiverse_high_contention_counter_with_opacity", stress_runner_accepts_multiverse_high_contention_counter_with_opacity},
        {"stress_runner_accepts_multiverse_snapshot_during_retry_contention_with_opacity", stress_runner_accepts_multiverse_snapshot_during_retry_contention_with_opacity},
        {"model_checker_accepts_multiverse_tiny_set_with_opacity", model_checker_accepts_multiverse_tiny_set_with_opacity},
        {"model_checker_accepts_multiverse_bank_with_opacity", model_checker_accepts_multiverse_bank_with_opacity},
        {"stress_runner_accepts_multiverse_bank_read_only_snapshot_with_opacity", stress_runner_accepts_multiverse_bank_read_only_snapshot_with_opacity},
        {"stress_runner_accepts_multiverse_bank_read_heavy_snapshot_with_opacity", stress_runner_accepts_multiverse_bank_read_heavy_snapshot_with_opacity},
        {"model_checker_accepts_multiverse_direct_snapshot_bank_with_opacity", model_checker_accepts_multiverse_direct_snapshot_bank_with_opacity},
        {"stress_runner_exercises_multiverse_direct_snapshot_bank_with_updaters", stress_runner_exercises_multiverse_direct_snapshot_bank_with_updaters},
        {"model_checker_accepts_multiverse_shared_array_with_opacity", model_checker_accepts_multiverse_shared_array_with_opacity},
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
