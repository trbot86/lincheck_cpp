#include <lincheck/multiverse_hooks.hpp>

#include "multiverse/multiverse.hpp"

#include <array>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

struct TxTinySet {
    static constexpr int empty = 0;
    static constexpr std::size_t capacity = 3;

    std::array<ns_multiverse::tx_field<int>, capacity> slots;

    TxTinySet() {
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
            for (std::size_t i = 0; i < values.size(); ++i) {
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

lincheck::TestSpec tiny_set_spec() {
    return lincheck::test<TxTinySet, SequentialTinySet>()
        .operation("add", &TxTinySet::add, &SequentialTinySet::add, lincheck::values<int>({1, 2, 3}))
        .operation("remove", &TxTinySet::remove, &SequentialTinySet::remove, lincheck::values<int>({1, 2, 3}))
        .operation("contains", &TxTinySet::contains, &SequentialTinySet::contains, lincheck::values<int>({1, 2, 3}))
        .sequential_state([](const SequentialTinySet& model) {
            return model.state_key();
        })
        .state_representation([](TxTinySet& set) {
            return set.debug_string();
        });
}

lincheck::Actor actor(std::size_t operation_index, std::string name, int key) {
    lincheck::Actor result;
    result.operation_index = operation_index;
    result.name = std::move(name);
    result.arguments.push_back(lincheck::Value(key));
    return result;
}

lincheck::ExecutionScenario representative_scenario() {
    lincheck::ExecutionScenario scenario;
    scenario.init = {
        actor(0, "add", 1)
    };
    scenario.parallel = {
        {actor(0, "add", 2)},
        {actor(1, "remove", 1)}
    };
    scenario.post = {
        actor(2, "contains", 1),
        actor(2, "contains", 2)
    };
    return scenario;
}

void require_success(const lincheck::CheckResult& result, const char* phase) {
    if (!result.success) {
        std::cerr << phase << " failed: " << result.message << "\n" << result.trace << "\n";
        throw std::runtime_error(phase);
    }
}

} // namespace

int main() {
    auto spec = tiny_set_spec();

    const auto model_result = lincheck::ModelCheckingOptions()
        .max_schedule_length(8)
        .max_context_switches_per_schedule(2)
        .check("multiverse-tiny-set-example", spec, representative_scenario());
    require_success(model_result, "model check");

    const auto stress_result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(4)
        .threads(2)
        .actors_per_thread(2)
        .actors_before(1)
        .actors_after(1)
        .seed(20260704)
        .check("multiverse-tiny-set-example", spec);
    require_success(stress_result, "stress check");

    std::cout << "[PASS] multiverse_tiny_set_example\n";
    return 0;
}
