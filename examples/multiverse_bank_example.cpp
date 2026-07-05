#include <lincheck/multiverse_hooks.hpp>

#include "multiverse/multiverse.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct TxBank {
    std::array<ns_multiverse::tx_field<int>, 2> balances;

    TxBank() {
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

lincheck::TestSpec bank_spec() {
    return lincheck::test<TxBank, SequentialBank>()
        .operation(
            "transfer_left_to_right",
            &TxBank::transfer_left_to_right,
            &SequentialBank::transfer_left_to_right,
            lincheck::values<int>({1, 2})
        )
        .operation(
            "transfer_right_to_left",
            &TxBank::transfer_right_to_left,
            &SequentialBank::transfer_right_to_left,
            lincheck::values<int>({1, 2})
        )
        .operation("total", &TxBank::total, &SequentialBank::total)
        .operation("left", &TxBank::left, &SequentialBank::left)
        .operation("right", &TxBank::right, &SequentialBank::right)
        .sequential_state([](const SequentialBank& model) {
            return model.state_key();
        })
        .state_representation([](TxBank& bank) {
            return bank.debug_string();
        });
}

lincheck::Actor actor(std::size_t operation_index, std::string name, std::vector<lincheck::Value> arguments = {}) {
    lincheck::Actor result;
    result.operation_index = operation_index;
    result.name = std::move(name);
    result.arguments = std::move(arguments);
    return result;
}

lincheck::ExecutionScenario representative_scenario() {
    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {actor(0, "transfer_left_to_right", {lincheck::Value(2)})},
        {actor(1, "transfer_right_to_left", {lincheck::Value(2)})}
    };
    scenario.post = {
        actor(2, "total"),
        actor(3, "left"),
        actor(4, "right")
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
    auto spec = bank_spec();

    const auto model_result = lincheck::ModelCheckingOptions()
        .max_schedule_length(64)
        .max_context_switches_per_schedule(0)
        .check("multiverse-bank-example", spec, representative_scenario());
    require_success(model_result, "model check");

    const auto stress_result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(2)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(1)
        .seed(20260705)
        .check("multiverse-bank-example", spec);
    require_success(stress_result, "stress check");

    std::cout << "[PASS] multiverse_bank_example\n";
    return 0;
}
