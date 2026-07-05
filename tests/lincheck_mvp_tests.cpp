#include <lincheck/lincheck.hpp>
#include <lincheck/multiverse_hooks.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct CustomResult {
    int code = 0;
    std::string label;

    friend bool operator==(const CustomResult&, const CustomResult&) = default;
};

inline std::ostream& operator<<(std::ostream& out, const CustomResult& value) {
    return out << "custom(" << value.code << "," << value.label << ")";
}

namespace std {
template <>
struct hash<CustomResult> {
    std::size_t operator()(const CustomResult& value) const noexcept {
        return static_cast<std::size_t>(value.code * 131) ^ std::hash<std::string>{}(value.label);
    }
};
} // namespace std

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_contains_in_order(const std::string& text, const std::vector<std::string>& needles, const std::string& message) {
    std::size_t position = 0;
    for (const auto& needle : needles) {
        const auto found = text.find(needle, position);
        if (found == std::string::npos) {
            throw std::runtime_error(message + ": missing " + needle);
        }
        position = found + needle.size();
    }
}

void require_exception_message(const std::exception_ptr& exception, const std::string& expected, const std::string& message) {
    require(exception != nullptr, message + ": missing exception");
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& e) {
        require(e.what() == expected, message + ": unexpected exception message");
        return;
    }
    throw std::runtime_error(message + ": non-std exception");
}

template <typename Fn>
void require_invalid_argument(Fn&& fn, const std::string& message, const std::string& expected_substring = {}) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::invalid_argument& e) {
        if (!expected_substring.empty()) {
            require(
                std::string(e.what()).find(expected_substring) != std::string::npos,
                message + ": unexpected invalid_argument message"
            );
        }
        return;
    }
    throw std::runtime_error(message + ": expected std::invalid_argument");
}

bool has_warning(const std::vector<std::string>& warnings, const std::string& first, const std::string& second = "") {
    return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& warning) {
        return warning.find(first) != std::string::npos &&
            (second.empty() || warning.find(second) != std::string::npos);
    });
}

bool has_warning(const lincheck::CheckResult& result, const std::string& first, const std::string& second = "") {
    return has_warning(result.warnings, first, second);
}

bool has_memory_event(const lincheck::CheckResult& result, lincheck::MemoryEventKind kind) {
    return std::any_of(result.memory_events.begin(), result.memory_events.end(), [&](const auto& record) {
        return record.event.kind == kind;
    });
}

bool has_memory_event_object_id(const lincheck::CheckResult& result, lincheck::MemoryEventKind kind) {
    return std::any_of(result.memory_events.begin(), result.memory_events.end(), [&](const auto& record) {
        return record.event.kind == kind &&
            record.object_id.rfind("obj#", 0) == 0 &&
            lincheck::to_string(record).find("object=" + record.object_id) != std::string::npos;
    });
}

bool has_memory_event_source_location(const lincheck::CheckResult& result, lincheck::MemoryEventKind kind) {
    return std::any_of(result.memory_events.begin(), result.memory_events.end(), [&](const auto& record) {
        return record.event.kind == kind &&
            record.has_source &&
            record.location_id.rfind("loc#", 0) == 0 &&
            lincheck::to_string(record).find("location=" + record.location_id) != std::string::npos &&
            lincheck::to_string(record).find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
}

bool has_source_access(const lincheck::CheckResult& result, lincheck::SourceAccessKind kind) {
    return std::any_of(result.source_accesses.begin(), result.source_accesses.end(), [&](const auto& record) {
        return record.event.kind == kind &&
            record.object_id.rfind("obj#", 0) == 0 &&
            record.location_id.rfind("loc#", 0) == 0 &&
            record.event.has_value &&
            lincheck::to_string(record).find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
}

bool has_synchronization_event(
    const std::vector<lincheck::SynchronizationEventRecord>& events,
    lincheck::SynchronizationEventKind kind
) {
    return std::any_of(events.begin(), events.end(), [&](const auto& record) {
        return record.event.kind == kind &&
            record.object_id.rfind("obj#", 0) == 0 &&
            lincheck::to_string(record).find("thread=") != std::string::npos &&
            lincheck::to_string(record).find("object=" + record.object_id) != std::string::npos;
    });
}

bool has_synchronization_event(const lincheck::CheckResult& result, lincheck::SynchronizationEventKind kind) {
    return has_synchronization_event(result.synchronization_events, kind);
}

bool has_event_dependency_node(
    const lincheck::CheckResult& result,
    const std::string& stream,
    const std::string& kind
) {
    return std::any_of(result.event_dependencies.nodes.begin(), result.event_dependencies.nodes.end(), [&](const auto& node) {
        return node.stream == stream &&
            node.kind == kind &&
            node.thread_id >= 0;
    });
}

bool has_event_dependency_edge(
    const lincheck::CheckResult& result,
    lincheck::EventDependencyEdgeKind kind,
    const std::string& resource_prefix = {}
) {
    return std::any_of(result.event_dependencies.edges.begin(), result.event_dependencies.edges.end(), [&](const auto& edge) {
        return edge.kind == kind &&
            (resource_prefix.empty() || edge.resource_id.rfind(resource_prefix, 0) == 0);
    });
}

struct BrokenCounter {
    lincheck::atomic<int> value{0};

    int inc() {
        const int observed = value.load();
        value.store(observed + 1);
        return observed + 1;
    }
};

struct StressBrokenCounter {
    lincheck::atomic<int> value{0};
    lincheck::atomic<int> loaded{0};

    int inc() {
        const int observed = value.load();
        loaded.fetch_add(1);
        while (loaded.load() < 2) {
            lincheck::this_thread::yield();
        }
        value.store(observed + 1);
        return observed + 1;
    }
};

struct AtomicCounter {
    lincheck::atomic<int> value{0};

    int inc() {
        return value.fetch_add(1) + 1;
    }
};

struct DisjointAtomicSlots {
    lincheck::atomic<int> left{0};
    lincheck::atomic<int> right{0};

    int touch_left() {
        LC_SWITCH();
        const int observed = left.load();
        left.store(observed + 1);
        return observed + 1;
    }

    int touch_right() {
        LC_SWITCH();
        const int observed = right.load();
        right.store(observed + 1);
        return observed + 1;
    }
};

struct SequentialDisjointAtomicSlots {
    int left = 0;
    int right = 0;

    int touch_left() {
        return ++left;
    }

    int touch_right() {
        return ++right;
    }

    std::string state_key() const {
        return std::to_string(left) + "," + std::to_string(right);
    }
};

struct StmHookLoadStoreCounter {
    std::atomic<int> value{0};

    int inc() {
        lincheck::stm::tx_begin(false);
        const int observed = value.load(std::memory_order_seq_cst);
        lincheck::stm::tx_read(&value, 1, static_cast<std::uint64_t>(observed));
        value.store(observed + 1, std::memory_order_seq_cst);
        lincheck::stm::tx_write(&value, 1);
        lincheck::stm::tx_commit_success(static_cast<std::uint64_t>(observed + 1));
        return observed + 1;
    }
};

struct RelaxedAtomicCounter {
    lincheck::atomic<int> value{0};

    int inc() {
        return value.fetch_add(1, std::memory_order_relaxed) + 1;
    }
};

struct ReplayScheduleProbe {
    int switch_once() {
        lincheck::switch_point("replay.probe.switch_once");
        return 1;
    }

    int no_switch() {
        return 2;
    }
};

struct SequentialReplayScheduleProbe {
    int switch_once() {
        return 1;
    }

    int no_switch() {
        return 2;
    }
};

struct FactoryCounter {
    lincheck::atomic<int> value;

    explicit FactoryCounter(int initial) : value(initial) {}

    int inc() {
        return value.fetch_add(1) + 1;
    }
};

struct AtomicAccumulator {
    lincheck::atomic<int> value{0};

    int add(int delta) {
        return value.fetch_add(delta) + delta;
    }
};

struct SequentialAccumulator {
    int value = 0;

    int add(int delta) {
        value += delta;
        return value;
    }
};

struct AppendLog {
    std::string value;

    void append(std::string text) {
        value += text;
    }

    std::string snapshot() const {
        return value;
    }
};

struct FactorySequentialCounter {
    int value;

    explicit FactorySequentialCounter(int initial) : value(initial) {}

    int inc() {
        return ++value;
    }
};

struct NonCopySequentialCounter {
    int value;

    explicit NonCopySequentialCounter(int initial) : value(initial) {}
    NonCopySequentialCounter(const NonCopySequentialCounter&) = delete;
    NonCopySequentialCounter& operator=(const NonCopySequentialCounter&) = delete;
    NonCopySequentialCounter(NonCopySequentialCounter&&) noexcept = default;
    NonCopySequentialCounter& operator=(NonCopySequentialCounter&&) noexcept = default;

    int inc() {
        return ++value;
    }
};

struct CustomResultAtomicCounter {
    lincheck::atomic<int> value{0};

    CustomResult inc() {
        return CustomResult{value.fetch_add(1) + 1, "inc"};
    }
};

struct CustomResultSequentialCounter {
    int value = 0;

    CustomResult inc() {
        return CustomResult{++value, "inc"};
    }
};

struct BrokenVarCounter {
    lincheck::var<int> value{0};

    int inc() {
        const int observed = value.get();
        value.set(observed + 1);
        return observed + 1;
    }
};

struct BrokenVarOperatorCounter {
    lincheck::var<int> value{0};

    int inc() {
        const int observed = value;
        value = observed + 1;
        return observed + 1;
    }
};

struct SourceMacroCounter {
    int value = 0;

    int inc() {
        const int observed = LC_READ(value);
        LC_WRITE(value, observed + 1);
        return observed + 1;
    }
};

struct SwitchOnlyOperation {
    int ping() {
        LC_SWITCH();
        return 1;
    }
};

struct SequentialSwitchOnlyOperation {
    int ping() {
        return 1;
    }
};

struct ObstructionSpin {
    int spin() {
        while (true) {
            lincheck::switch_point("obstruction.spin");
        }
        return 0;
    }
};

struct ObstructionPark {
    lincheck::parker parker;

    int wait() {
        parker.park();
        return 1;
    }
};

struct ObstructionAtomicWait {
    lincheck::atomic<int> value{0};

    int wait() {
        value.wait(0);
        return 1;
    }
};

struct ObstructionSemaphoreWait {
    lincheck::binary_semaphore semaphore{0};

    int wait() {
        semaphore.acquire();
        return 1;
    }
};

struct ObstructionLatchWait {
    lincheck::latch latch{1};

    int wait() {
        latch.wait();
        return 1;
    }
};

struct ObstructionBarrierWait {
    lincheck::barrier<> barrier{2};

    int wait() {
        barrier.arrive_and_wait();
        return 1;
    }
};

struct TimeoutSpin {
    int spin() {
        while (true) {
            lincheck::switch_point("timeout.spin");
        }
        return 0;
    }
};

struct ThrowingOperation {
    int fail() {
        throw std::runtime_error("operation boom");
    }
};

struct EmptyQueueError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct LockedCounter {
    lincheck::mutex mutex;
    int value = 0;

    int inc() {
        lincheck::lock_guard lock(mutex);
        return ++value;
    }
};

struct SequentialCounter {
    int value = 0;

    int inc() {
        return ++value;
    }
};

struct WrongSequentialCounter {
    int inc() {
        return 0;
    }
};

struct BrokenSingleSlotQueue {
    lincheck::atomic<int> occupied{0};
    int slot = 0;

    bool push(int value) {
        if (occupied.load() != 0) return false;
        slot = value;
        occupied.store(1);
        return true;
    }
};

struct StressBrokenSingleSlotQueue {
    lincheck::atomic<int> occupied{0};
    lincheck::atomic<int> checked_empty{0};
    int slot = 0;

    bool push(int value) {
        if (occupied.load() != 0) return false;
        checked_empty.fetch_add(1);
        while (checked_empty.load() < 2) {
            lincheck::this_thread::yield();
        }
        slot = value;
        occupied.store(1);
        return true;
    }
};

struct SequentialSingleSlotQueue {
    bool occupied = false;
    int slot = 0;

    bool push(int value) {
        if (occupied) return false;
        slot = value;
        occupied = true;
        return true;
    }
};

struct SequentialStack {
    std::vector<int> items;

    void push(int value) {
        items.push_back(value);
    }

    int pop() {
        if (items.empty()) return -1;
        const int value = items.back();
        items.pop_back();
        return value;
    }
};

struct SequentialQueue {
    std::deque<int> items;

    void push(int value) {
        items.push_back(value);
    }

    int pop() {
        if (items.empty()) return -1;
        const int value = items.front();
        items.pop_front();
        return value;
    }
};

struct OptionalLockedQueue {
    lincheck::mutex mutex;
    std::deque<int> items;

    OptionalLockedQueue() = default;

    explicit OptionalLockedQueue(int initial) {
        items.push_back(initial);
    }

    std::optional<int> pop() {
        std::lock_guard lock(mutex);
        if (items.empty()) return std::nullopt;
        const int value = items.front();
        items.pop_front();
        return value;
    }
};

struct OptionalSequentialQueue {
    std::deque<int> items;

    OptionalSequentialQueue() = default;

    explicit OptionalSequentialQueue(int initial) {
        items.push_back(initial);
    }

    std::optional<int> pop() {
        if (items.empty()) return std::nullopt;
        const int value = items.front();
        items.pop_front();
        return value;
    }
};

struct ThrowingPopQueue {
    lincheck::mutex mutex;
    std::deque<int> items;

    ThrowingPopQueue() = default;

    explicit ThrowingPopQueue(int initial) {
        items.push_back(initial);
    }

    int pop() {
        std::lock_guard lock(mutex);
        if (items.empty()) throw EmptyQueueError("empty");
        const int value = items.front();
        items.pop_front();
        return value;
    }
};

struct ThrowingPopSequentialQueue {
    std::deque<int> items;

    ThrowingPopSequentialQueue() = default;

    explicit ThrowingPopSequentialQueue(int initial) {
        items.push_back(initial);
    }

    int pop() {
        if (items.empty()) throw EmptyQueueError("empty");
        const int value = items.front();
        items.pop_front();
        return value;
    }
};

struct ReturningEmptySequentialQueue {
    int pop() {
        return -1;
    }
};

struct GroupedOperations {
    int first() { return 1; }
    int second() { return 2; }
    int ordinary() { return 3; }
};

enum class GeneratedChoice : unsigned {
    first = 1,
    second = 2
};

struct GeneratedParameters {
    int record(bool flag, std::string text, GeneratedChoice choice, int value) {
        return (flag ? 100 : 0) +
            static_cast<int>(text.size()) +
            static_cast<int>(choice) * 10 +
            value;
    }
};

struct RecordingRuntime : lincheck::Runtime {
    void switch_point(const char* location) override {
        std::lock_guard lock(mutex);
        switches.emplace_back(location);
    }

    void event(const std::string& description) override {
        std::lock_guard lock(mutex);
        events.push_back(description);
    }

    void warning(const std::string& message) override {
        std::lock_guard lock(mutex);
        warnings.push_back(message);
    }

    void memory_event(const lincheck::MemoryEvent& event) override {
        std::lock_guard lock(mutex);
        memory_events.push_back(event);
    }

    void stm_event(const lincheck::stm::Event& event) override {
        std::lock_guard lock(mutex);
        stm_events.push_back(event);
        events.push_back(lincheck::stm::to_string(event));
        switches.push_back(lincheck::stm::switch_location(event.kind));
    }

    void source_access_event(const lincheck::SourceAccessEvent& event) override {
        std::lock_guard lock(mutex);
        source_accesses.push_back(event);
    }

    void synchronization_event(const lincheck::SynchronizationEvent& event) override {
        std::lock_guard lock(mutex);
        synchronization_events.push_back(event);
    }

    std::mutex mutex;
    std::vector<std::string> switches;
    std::vector<std::string> events;
    std::vector<std::string> warnings;
    std::vector<lincheck::MemoryEvent> memory_events;
    std::vector<lincheck::stm::Event> stm_events;
    std::vector<lincheck::SourceAccessEvent> source_accesses;
    std::vector<lincheck::SynchronizationEvent> synchronization_events;
};

void model_checker_finds_broken_counter() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(!result.success, "model checker should find broken counter");
    require(result.failure == lincheck::FailureKind::invalid_results, "broken counter should have invalid-results failure kind");
    require(result.message == "invalid execution results", "broken counter should fail verification");
    require(result.trace.find("failure:\n  kind: invalid_results") != std::string::npos, "broken counter trace should include failure summary");
    require(result.trace.find("schedule:") != std::string::npos, "broken counter failure should include schedule");
    require(result.trace.find("operation.start") != std::string::npos, "broken counter failure should include operation start events");
    require(result.trace.find("operation.finish") != std::string::npos, "broken counter failure should include operation finish events");
    require(result.trace.find("atomic.load") != std::string::npos, "broken counter failure should include tracked load");
    require(result.trace.find("trace events:\n") != std::string::npos, "broken counter trace should include structured trace events");
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.kind == "operation.start" &&
                record.operation &&
                record.operation->name == "inc" &&
                lincheck::to_string(record).find("operation=inc@0#0") != std::string::npos;
        }),
        "model-checker trace events should retain active public operation context"
    );
    require(result.trace.find("memory events:\n") != std::string::npos, "broken counter trace should include structured memory events");
    require(result.trace.find("atomic.load order=seq_cst") != std::string::npos, "memory event section should include load order");
    require(result.trace.find("object=obj#") != std::string::npos, "memory event section should include stable object IDs");
    require(result.trace.find("operation=inc@0#0") != std::string::npos, "memory event section should include operation context");
    require(
        std::any_of(result.memory_events.begin(), result.memory_events.end(), [](const auto& record) {
            return record.operation &&
                record.operation->name == "inc" &&
                lincheck::to_string(record).find("operation=inc@0#0") != std::string::npos;
        }),
        "model-checker memory events should retain the active public operation context"
    );
    require(!result.event_dependencies.nodes.empty(), "broken counter should retain event dependency graph nodes");
    require(
        has_event_dependency_node(result, "trace", "operation.start"),
        "event dependency graph should include generic trace event nodes"
    );
    require(
        has_event_dependency_node(result, "memory", "atomic.load"),
        "event dependency graph should include memory event nodes"
    );
    require(
        std::any_of(result.event_dependencies.nodes.begin(), result.event_dependencies.nodes.end(), [](const auto& node) {
            return node.stream == "memory" &&
                node.operation &&
                node.operation->name == "inc" &&
                lincheck::to_string(node).find("operation=inc@0#0") != std::string::npos;
        }),
        "event dependency nodes should retain active public operation context"
    );
    require(
        !result.operation_dependency_footprints.empty(),
        "broken counter should expose per-operation dependency footprints"
    );
    require(
        std::any_of(
            result.operation_dependency_footprints.begin(),
            result.operation_dependency_footprints.end(),
            [](const auto& footprint) {
                return footprint.operation.name == "inc" &&
                    footprint.event_count >= 2 &&
                    std::find(footprint.streams.begin(), footprint.streams.end(), "trace") != footprint.streams.end() &&
                    std::find(footprint.streams.begin(), footprint.streams.end(), "memory") != footprint.streams.end() &&
                    std::any_of(footprint.resources.begin(), footprint.resources.end(), [](const auto& resource) {
                        return resource.rfind("obj#", 0) == 0;
                    }) &&
                    lincheck::to_string(footprint).find("operation=") == std::string::npos;
            }
        ),
        "operation dependency footprints should summarize active operation streams and resources"
    );
    require(
        has_event_dependency_edge(result, lincheck::EventDependencyEdgeKind::stream_resource_order, "obj#"),
        "event dependency graph should include same-object resource edges"
    );
    require(
        has_event_dependency_edge(result, lincheck::EventDependencyEdgeKind::cross_stream_thread_order),
        "event dependency graph should connect trace and memory streams by thread order"
    );
    require(result.trace.find("event dependencies:\n") != std::string::npos, "broken counter trace should include event dependency graph");
    require(result.event_dependency_analysis.consistent, "generated event dependency graph should be consistent");
    require(
        result.event_dependency_analysis.topological_order.size() == result.event_dependencies.nodes.size(),
        "event dependency analysis should cover all graph nodes"
    );
    require(
        result.trace.find("event dependency analysis:\n") != std::string::npos,
        "broken counter trace should include event dependency analysis"
    );
    require(
        result.trace.find("operation dependency footprints:\n") != std::string::npos,
        "broken counter trace should include operation dependency footprints"
    );
    require(
        result.trace.find("streams=memory") != std::string::npos,
        "operation dependency footprint trace should include touched streams"
    );
    require(result.trace.find("interleaving:\n") != std::string::npos, "broken counter trace should include interleaving section");
    require(result.trace.find("thread interleaving:\n") != std::string::npos, "broken counter trace should include thread-by-thread interleaving section");
    require(result.trace.find("  thread 0:\n    actor 0 inc()") != std::string::npos, "thread interleaving should group actors by thread");
    require(result.trace.find("thread 0 actor 0") != std::string::npos, "broken counter interleaving should name thread actors");
    require(!result.verifier_explanation.empty(), "broken counter failure should include verifier explanation field");
    require(result.trace.find("verifier explanation:") != std::string::npos, "broken counter trace should include verifier explanation section");
    require(
        result.verifier_explanation.find("no legal sequential ordering found") != std::string::npos,
        "broken counter verifier explanation should describe the failed linearization search"
    );
    require(result.trace.find("model-checking stats:\n") != std::string::npos, "broken counter trace should include model-checking stats");
    require(
        result.trace.find("schedules_explored=" + std::to_string(result.stats.schedules_explored)) != std::string::npos,
        "model-checking stats should include explored schedule count"
    );
    require(
        result.trace.find(
            "max_context_switch_depth_explored=" +
            std::to_string(result.stats.max_context_switch_depth_explored)
        ) != std::string::npos,
        "model-checking stats should include max context-switch depth"
    );
    require(
        result.trace.find("retained_schedule_decisions=" + std::to_string(result.schedule_decisions.size())) != std::string::npos,
        "model-checking stats should include retained scheduling-point count"
    );
    require(
        lincheck::format_model_checking_stats(result).find("schedules_explored=") != std::string::npos,
        "model-checking stats should be available for caller logging"
    );
    require_contains_in_order(
        result.trace,
        {
            "schedule:",
            "failure:",
            "trace events:",
            "memory events:",
            "event dependencies:",
            "event dependency analysis:",
            "operation dependency footprints:",
            "verifier explanation:",
            "interleaving:",
            "thread interleaving:",
            "operation clocks:",
            "parallel:",
            "model-checking stats:"
        },
        "broken counter trace should have stable sections"
    );
}

void event_dependency_analysis_detects_invalid_edges_and_cycles() {
    auto node = [](std::size_t index, std::string stream, std::string kind) {
        return lincheck::EventDependencyNode{
            .index = index,
            .stream = std::move(stream),
            .sequence = index,
            .event_index = index,
            .thread_id = 0,
            .operation = std::nullopt,
            .kind = std::move(kind),
            .resource_id = {},
            .related_resource_id = {}
        };
    };
    auto edge = [](std::size_t from, std::size_t to) {
        return lincheck::EventDependencyEdge{
            .from = from,
            .to = to,
            .kind = lincheck::EventDependencyEdgeKind::stream_thread_order,
            .resource_id = {}
        };
    };

    lincheck::EventDependencyGraph invalid;
    invalid.nodes.push_back(node(0, "memory", "atomic.load"));
    invalid.edges.push_back(edge(0, 7));
    const auto invalid_report = lincheck::analyze_event_dependency_graph(invalid);
    require(!invalid_report.consistent, "dependency analysis should reject missing edge endpoints");
    require(
        invalid_report.explanation.find("missing node") != std::string::npos,
        "missing-endpoint analysis should explain the invalid edge"
    );

    lincheck::EventDependencyGraph cyclic;
    cyclic.nodes.push_back(node(0, "memory", "atomic.load"));
    cyclic.nodes.push_back(node(1, "memory", "atomic.store"));
    cyclic.edges.push_back(edge(0, 1));
    cyclic.edges.push_back(edge(1, 0));
    const auto cyclic_report = lincheck::analyze_event_dependency_graph(cyclic);
    require(!cyclic_report.consistent, "dependency analysis should reject cycles");
    require(
        cyclic_report.explanation.find("cycle") != std::string::npos,
        "cycle analysis should explain unresolved dependencies"
    );

    lincheck::EventDependencyGraph acyclic;
    acyclic.nodes.push_back(node(0, "source", "source.read"));
    acyclic.nodes.push_back(node(1, "source", "source.write"));
    acyclic.edges.push_back(edge(0, 1));
    const auto acyclic_report = lincheck::analyze_event_dependency_graph(acyclic);
    require(acyclic_report.consistent, "dependency analysis should accept acyclic graphs");
    require(acyclic_report.topological_order.size() == 2, "acyclic analysis should return a topological order");
    require(
        lincheck::to_string(acyclic.edges.front()).find("stream_thread_order") != std::string::npos,
        "dependency edge formatting should include the edge kind"
    );

    acyclic.nodes.front().kind = "source.read\"quoted";
    acyclic.nodes.front().resource_id = "loc#1";
    acyclic.edges.front().resource_id = "loc#1";
    const auto json = lincheck::format_event_dependency_graph_json(acyclic, &acyclic_report);
    require(
        json.find("\"nodes\"") != std::string::npos && json.find("\"analysis\"") != std::string::npos,
        "dependency graph JSON export should include nodes and analysis"
    );
    require(
        json.find("source.read\\\"quoted") != std::string::npos,
        "dependency graph JSON export should escape strings"
    );
    require(
        json.find("\"topological_order\": [0, 1]") != std::string::npos,
        "dependency graph JSON export should include the topological order"
    );

    const auto dot = lincheck::format_event_dependency_graph_dot(acyclic, "deps graph");
    require(
        dot.find("digraph deps_graph") != std::string::npos,
        "dependency graph DOT export should sanitize the graph name"
    );
    require(
        dot.find("n0 -> n1") != std::string::npos && dot.find("stream_thread_order") != std::string::npos,
        "dependency graph DOT export should include labeled edges"
    );

    lincheck::MemoryEvent memory_event;
    memory_event.kind = lincheck::MemoryEventKind::atomic_load;
    memory_event.object = reinterpret_cast<const void*>(0x1);
    lincheck::SourceAccessEvent source_event;
    source_event.kind = lincheck::SourceAccessKind::read;
    source_event.object = reinterpret_cast<const void*>(0x1);
    source_event.source = lincheck::source_location("dep.cpp", 7, "probe");

    const auto cross_stream = lincheck::build_event_dependency_graph(
        {lincheck::detail::make_trace_event_record(0, 0, "operation.start", "operation.start probe", 0)},
        {lincheck::detail::make_memory_event_record(0, 0, memory_event, 0)},
        {},
        {lincheck::detail::make_source_access_event_record(0, 0, source_event, 1)},
        {}
    );
    require(
        std::any_of(cross_stream.nodes.begin(), cross_stream.nodes.end(), [](const auto& dependency_node) {
            return dependency_node.stream == "trace" && dependency_node.kind == "operation.start";
        }),
        "dependency graph should include trace event nodes"
    );
    require(
        std::any_of(cross_stream.edges.begin(), cross_stream.edges.end(), [](const auto& dependency_edge) {
            return dependency_edge.kind == lincheck::EventDependencyEdgeKind::cross_stream_thread_order;
        }),
        "dependency graph should connect same-thread events across streams by event index"
    );
    require(
        std::any_of(cross_stream.edges.begin(), cross_stream.edges.end(), [](const auto& dependency_edge) {
            return dependency_edge.kind == lincheck::EventDependencyEdgeKind::cross_stream_resource_order &&
                dependency_edge.resource_id.rfind("obj#", 0) == 0;
        }),
        "dependency graph should connect same-resource events across streams by event index"
    );
    require(
        lincheck::format_event_dependency_graph_json(cross_stream).find("\"event_index\": 1") != std::string::npos,
        "dependency graph JSON export should include unified event indexes"
    );
}

void model_checker_replays_captured_schedule() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto options = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .minimize_failed_scenario(false);

    const auto original = options.check(spec);
    require(!original.success, "broken counter should produce a replayable failure");
    require(!original.schedule.empty(), "model-check failure should expose the explored schedule");

    std::ostringstream schedule_line;
    schedule_line << "schedule:";
    for (const int choice : original.schedule) {
        schedule_line << " " << choice;
    }
    schedule_line << "\n";
    require(original.trace.find(schedule_line.str()) != std::string::npos, "failure trace should include the exposed schedule");

    const auto parsed_line_schedule = lincheck::parse_schedule_line(schedule_line.str());
    require(parsed_line_schedule.has_value(), "schedule line should parse back into a schedule vector");
    require(*parsed_line_schedule == original.schedule, "parsed schedule line should match the exposed schedule");
    const auto parsed_trace_schedule = lincheck::schedule_from_trace(original.trace);
    require(parsed_trace_schedule.has_value(), "failure trace should parse back into a schedule vector");
    require(*parsed_trace_schedule == original.schedule, "parsed trace schedule should match the exposed schedule");
    const auto parsed_trace_decisions = lincheck::schedule_decisions_from_trace(original.trace);
    require(parsed_trace_decisions.has_value(), "failure trace should parse back into schedule decisions");
    require(parsed_trace_decisions->size() == original.schedule_decisions.size(), "parsed schedule decisions should match exposed decisions");
    require(!parsed_trace_decisions->empty(), "parsed schedule decisions should not be empty");
    require((*parsed_trace_decisions)[0].switch_position == 0, "parsed schedule decisions should preserve switch positions");
    require((*parsed_trace_decisions)[0].chosen_thread == original.schedule[0], "parsed schedule decisions should preserve chosen threads");
    require(
        original.trace.find(" operations:") != std::string::npos,
        "failure trace should include runnable operation context metadata"
    );
    const auto has_operation_context = std::any_of(
        original.schedule_decisions.begin(),
        original.schedule_decisions.end(),
        [](const lincheck::ScheduleDecision& decision) {
            return std::any_of(
                decision.runnable_operations.begin(),
                decision.runnable_operations.end(),
                [](const lincheck::OperationContext& context) {
                    return context.name == "inc" &&
                        context.actor_index == 0 &&
                        context.actor_label.find("inc(") != std::string::npos;
                }
            );
        }
    );
    require(has_operation_context, "schedule decisions should retain typed runnable operation context");

    const auto replayed = options.replay(spec, original.scenario, *parsed_trace_schedule);
    require(!replayed.success, "replayed schedule should reproduce the failure");
    require(replayed.failure == original.failure, "replayed schedule should preserve the failure kind");
    require(replayed.message == original.message, "replayed schedule should preserve the failure message");
    require(replayed.schedule == original.schedule, "replayed result should retain the replayed schedule");
    require(replayed.trace.find(schedule_line.str()) != std::string::npos, "replayed trace should include the replayed schedule");
    require(replayed.verifier_explanation.find("no legal sequential ordering found") != std::string::npos, "replayed failure should include verifier explanation");
    require(replayed.stats.schedules_generated == 1, "replay should report one generated schedule");
    require(replayed.stats.schedules_explored == 1, "replay should report one explored schedule");
    require(
        replayed.trace.find("model-checking stats:\n") != std::string::npos &&
            replayed.trace.find("schedules_explored=1") != std::string::npos,
        "replayed failure trace should include single-schedule stats"
    );

    const auto decision_replayed = options.replay(spec, original.scenario, *parsed_trace_decisions);
    require(!decision_replayed.success, "decision replay should reproduce the failure");
    require(decision_replayed.failure == original.failure, "decision replay should preserve the failure kind");
    require(decision_replayed.schedule == original.schedule, "decision replay should retain the chosen-thread schedule");
    require(
        decision_replayed.schedule_decisions.size() == parsed_trace_decisions->size(),
        "decision replay should retain the replayed switch-position decisions"
    );
    require(
        decision_replayed.schedule_decisions[0].location == (*parsed_trace_decisions)[0].location,
        "decision replay should validate switch locations"
    );
    const auto direct_decision_replayed = options.replay(spec, original.scenario, original.schedule_decisions);
    require(!direct_decision_replayed.success, "direct decision replay should reproduce the failure");
    require(
        std::any_of(
            direct_decision_replayed.schedule_decisions.begin(),
            direct_decision_replayed.schedule_decisions.end(),
            [](const lincheck::ScheduleDecision& decision) {
                return !decision.runnable_operations.empty();
            }
        ),
        "direct decision replay should retain operation context metadata"
    );

    const auto named = options.replay("broken-counter-replay", spec, original.scenario, original.schedule);
    require(named.test_name == "broken-counter-replay", "named replay should record the test name");
    require(named.trace.rfind("test: broken-counter-replay\n", 0) == 0, "named replay should prefix failure traces");
    const auto parsed_named_trace_schedule = lincheck::schedule_from_trace(named.trace);
    require(parsed_named_trace_schedule.has_value(), "named replay trace should parse the schedule after the test prefix");
    require(*parsed_named_trace_schedule == original.schedule, "named replay trace schedule should match the replayed schedule");
    const auto named_decision_replay = options.replay("broken-counter-decision-replay", spec, original.scenario, *parsed_trace_decisions);
    require(named_decision_replay.test_name == "broken-counter-decision-replay", "named decision replay should record the test name");

    require_invalid_argument(
        [&] { options.replay(spec, original.scenario, std::vector<int>{}); },
        "replay should reject an empty schedule",
        "schedule"
    );
    require_invalid_argument(
        [&] { options.replay(spec, original.scenario, std::vector<int>{static_cast<int>(original.scenario.parallel.size())}); },
        "replay should reject a schedule choice outside the scenario thread range",
        "outside"
    );
    require(original.schedule.size() > 1, "broken counter replay schedule should have enough choices for stale-schedule checks");
    auto shortened_schedule = original.schedule;
    shortened_schedule.pop_back();
    require_invalid_argument(
        [&] { options.replay(spec, original.scenario, shortened_schedule); },
        "replay should reject a schedule that ends before the execution is fully described",
        "ended before"
    );
    auto extended_schedule = original.schedule;
    extended_schedule.push_back(0);
    require_invalid_argument(
        [&] { options.replay(spec, original.scenario, extended_schedule); },
        "replay should reject a schedule with unused trailing choices",
        "unused"
    );
    auto stale_decisions = *parsed_trace_decisions;
    stale_decisions[0].location = "stale.location";
    require_invalid_argument(
        [&] { options.replay(spec, original.scenario, stale_decisions); },
        "decision replay should reject stale switch-location metadata",
        "location mismatch"
    );
    auto non_runnable_decisions = *parsed_trace_decisions;
    non_runnable_decisions[0].runnable_threads.clear();
    require_invalid_argument(
        [&] { options.replay(spec, original.scenario, non_runnable_decisions); },
        "decision replay should reject a chosen thread missing from the runnable set",
        "not listed as runnable"
    );
    lincheck::ExecutionScenario invalid_scenario;
    invalid_scenario.parallel = {{}};
    require_invalid_argument(
        [&] { options.replay(spec, invalid_scenario, std::vector<int>{0}); },
        "replay should reject invalid scenarios",
        "scenario"
    );

    require(!lincheck::parse_schedule_line("schedule: 0 x 1"), "malformed schedule lines should be rejected");
    require(!lincheck::parse_schedule_line("schedule: 0 -1 1"), "negative schedule choices should be rejected");
    require(!lincheck::parse_schedule_line("schedule:"), "empty schedule lines should be rejected");
    require(!lincheck::schedule_from_trace("failure only\nno schedule here\n"), "traces without schedules should not parse a schedule");
    require(
        !lincheck::parse_schedule_decision_line("#0 thread x @ somewhere -> 0 runnable: 0"),
        "malformed decision lines should be rejected"
    );
    const auto parsed_with_operations = lincheck::parse_schedule_decision_line(
        "#0 thread 0 @ atomic.load -> 1 runnable: 0 1 operations: 0=inc@0#0 1=inc@0#0"
    );
    require(parsed_with_operations.has_value(), "decision parser should ignore optional operation context suffixes");
    require(parsed_with_operations->runnable_threads.size() == 2, "decision parser should preserve runnable choices before operation suffixes");
    require(
        !lincheck::schedule_decisions_from_trace("schedule decisions:\n  #1 initial -> 0 runnable: 0\n"),
        "decision traces with non-contiguous positions should be rejected"
    );
}

void model_checker_replay_rejects_non_runnable_thread_choices() {
    lincheck::TestSpec spec = lincheck::test<ReplayScheduleProbe, SequentialReplayScheduleProbe>()
        .operation("switch_once", &ReplayScheduleProbe::switch_once, &SequentialReplayScheduleProbe::switch_once)
        .operation("no_switch", &ReplayScheduleProbe::no_switch, &SequentialReplayScheduleProbe::no_switch);

    const lincheck::Actor switch_once{
        .operation_index = 0,
        .name = "switch_once",
        .group = "",
        .non_parallel = false,
        .one_shot = false,
        .exception_results = false,
        .arguments = {}
    };
    const lincheck::Actor no_switch{
        .operation_index = 1,
        .name = "no_switch",
        .group = "",
        .non_parallel = false,
        .one_shot = false,
        .exception_results = false,
        .arguments = {}
    };

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {switch_once},
        {no_switch}
    };

    const auto valid_replay = lincheck::ModelCheckingOptions()
        .max_schedule_length(2)
        .replay(spec, scenario, std::vector<int>{1, 0});
    require(valid_replay.success, "replay probe should accept the exact runnable schedule");

    require_invalid_argument(
        [&] {
            lincheck::ModelCheckingOptions()
                .max_schedule_length(2)
                .replay(spec, scenario, std::vector<int>{1, 1});
        },
        "replay should reject a schedule choice for a finished thread",
        "not runnable"
    );
}

void model_checker_checks_hand_authored_scenario() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(100)
        .threads(1)
        .actors_per_thread(9)
        .actors_before(9)
        .actors_after(9)
        .max_schedule_length(3)
        .minimize_failed_scenario(false)
        .check("manual-broken-counter", spec, scenario);

    require(!result.success, "model checker should reject hand-authored broken scenario");
    require(result.test_name == "manual-broken-counter", "named hand-authored model check should record the test name");
    require(result.stats.scenarios_generated == 1, "hand-authored model check should report one scenario");
    require(result.scenario.to_string() == scenario.to_string(), "hand-authored model check should use the provided scenario");
    require(result.scenario.parallel.size() == 2, "hand-authored model check should ignore the configured generation thread count");
    require(!result.schedule.empty(), "hand-authored model check failure should expose the failing schedule");
    require(result.trace.rfind("test: manual-broken-counter\n", 0) == 0, "named hand-authored model check should prefix the trace");
}

void stress_runner_checks_hand_authored_scenario() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    scenario.init = {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}};
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };
    scenario.post = {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}};

    const auto result = lincheck::StressOptions()
        .iterations(2)
        .invocations_per_iteration(2)
        .threads(99)
        .actors_per_thread(99)
        .actors_before(99)
        .actors_after(99)
        .check("manual-atomic-counter", spec, scenario);

    require(result.success, "stress runner should accept hand-authored scenario");
    require(result.test_name == "manual-atomic-counter", "named hand-authored stress check should record the test name");
    require(result.stats.scenarios_generated == 1, "hand-authored stress check should report one scenario");
    require(result.scenario.to_string() == scenario.to_string(), "hand-authored stress check should keep the provided scenario on success");
    require(
        lincheck::format_model_checking_stats(result).empty(),
        "model-checking stats formatter should stay empty for stress-only results"
    );
}

void explicit_scenario_validation_rejects_bad_actors() {
    lincheck::TestSpec counter_spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario bad_index;
    bad_index.parallel = {
        {lincheck::Actor{.operation_index = 7, .name = "missing", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    require_invalid_argument(
        [&] { lincheck::ModelCheckingOptions().check(counter_spec, bad_index); },
        "model checker should reject scenario actors with invalid operation indexes",
        "operation_index"
    );
    require_invalid_argument(
        [&] { lincheck::StressOptions().check(counter_spec, bad_index); },
        "stress runner should reject scenario actors with invalid operation indexes",
        "operation_index"
    );
    require_invalid_argument(
        [&] { lincheck::ModelCheckingOptions().replay(counter_spec, bad_index, std::vector<int>{0}); },
        "replay should reject scenario actors with invalid operation indexes",
        "operation_index"
    );

    lincheck::ExecutionResult bad_index_result;
    bad_index_result.parallel_results = {{lincheck::Value(1)}};
    require_invalid_argument(
        [&] { lincheck::LinearizabilityVerifier(counter_spec).verify(bad_index, bad_index_result); },
        "standalone verifier should reject scenario actors with invalid operation indexes",
        "operation_index"
    );

    lincheck::TestSpec stack_spec = lincheck::test<SequentialStack, SequentialStack>()
        .operation("push", &SequentialStack::push, &SequentialStack::push, lincheck::range<int>(0, 9));

    lincheck::ExecutionScenario bad_arguments;
    bad_arguments.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    require_invalid_argument(
        [&] { lincheck::ModelCheckingOptions().check(stack_spec, bad_arguments); },
        "model checker should reject scenario actors with wrong argument counts",
        "arguments"
    );
    require_invalid_argument(
        [&] { lincheck::StressOptions().check(stack_spec, bad_arguments); },
        "stress runner should reject scenario actors with wrong argument counts",
        "arguments"
    );
    require_invalid_argument(
        [&] { lincheck::ModelCheckingOptions().replay(stack_spec, bad_arguments, std::vector<int>{0}); },
        "replay should reject scenario actors with wrong argument counts",
        "arguments"
    );

    lincheck::ExecutionResult bad_arguments_result;
    bad_arguments_result.parallel_results = {{lincheck::Value()}};
    require_invalid_argument(
        [&] { (void)lincheck::LinearizabilityVerifier(stack_spec).verify_with_report(bad_arguments, bad_arguments_result); },
        "standalone verifier should reject scenario actors with wrong argument counts",
        "arguments"
    );

    lincheck::TestSpec one_shot_spec = lincheck::test<GroupedOperations, GroupedOperations>()
        .operation_with_options(
            "ordinary",
            &GroupedOperations::ordinary,
            &GroupedOperations::ordinary,
            lincheck::one_shot()
        );

    lincheck::ExecutionScenario repeated_one_shot;
    repeated_one_shot.init = {
        lincheck::Actor{.operation_index = 0, .name = "ordinary", .group = "", .non_parallel = false, .one_shot = true, .arguments = {}}
    };
    repeated_one_shot.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "ordinary", .group = "", .non_parallel = false, .one_shot = true, .arguments = {}}}
    };

    require_invalid_argument(
        [&] { lincheck::ModelCheckingOptions().check(one_shot_spec, repeated_one_shot); },
        "model checker should reject hand-authored scenarios that repeat one-shot operations",
        "one-shot"
    );

    lincheck::TestSpec grouped_spec = lincheck::test<GroupedOperations, GroupedOperations>()
        .operation_with_options(
            "first",
            &GroupedOperations::first,
            &GroupedOperations::first,
            lincheck::non_parallel_group("exclusive")
        )
        .operation_with_options(
            "second",
            &GroupedOperations::second,
            &GroupedOperations::second,
            lincheck::non_parallel_group("exclusive")
        );

    lincheck::ExecutionScenario split_non_parallel_group;
    split_non_parallel_group.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "first", .group = "exclusive", .non_parallel = true, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "second", .group = "exclusive", .non_parallel = true, .one_shot = false, .arguments = {}}}
    };

    require_invalid_argument(
        [&] { lincheck::StressOptions().check(grouped_spec, split_non_parallel_group); },
        "stress runner should reject hand-authored scenarios that split non-parallel groups",
        "non-parallel group"
    );
    lincheck::ExecutionResult split_results;
    split_results.parallel_results = {{lincheck::Value(1)}, {lincheck::Value(2)}};
    require_invalid_argument(
        [&] { (void)lincheck::LinearizabilityVerifier(grouped_spec).verify(split_non_parallel_group, split_results); },
        "standalone verifier should reject split non-parallel groups",
        "non-parallel group"
    );
}

void model_checker_uses_stm_hooks_as_switch_points() {
    lincheck::TestSpec spec = lincheck::test<StmHookLoadStoreCounter, SequentialCounter>()
        .operation("inc", &StmHookLoadStoreCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(5)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "STM hooks should drive model-checker interleavings");
    require(result.failure == lincheck::FailureKind::invalid_results, "STM-hook load/store counter should fail linearizability");
    require(result.trace.find("stm.tx_read") != std::string::npos, "STM-hook failure should include read hook trace");
    require(result.trace.find("stm.tx_write") != std::string::npos, "STM-hook failure should include write hook trace");
    require(result.trace.find("stm events:\n") != std::string::npos, "STM-hook failure should include structured STM event section");
    require(!result.stm_events.empty(), "STM-hook failure should retain structured STM event records");
    require(
        std::any_of(result.stm_events.begin(), result.stm_events.end(), [](const auto& record) {
            return record.kind == "tx_read" &&
                record.address_id.rfind("obj#", 0) == 0 &&
                record.has_lock_slot &&
                record.lock_slot == 1 &&
                record.has_version &&
                record.version == 0 &&
                record.transaction_id != 0 &&
                record.transaction_depth == 1 &&
                record.operation &&
                record.operation->name == "inc" &&
                lincheck::to_string(record).find("stm.tx_read") != std::string::npos;
        }),
        "STM event records should retain read metadata and stable address IDs"
    );
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.kind == "switch-point" &&
                record.description == "switch-point stm.tx_read" &&
                record.transaction_id != 0 &&
                record.transaction_depth == 1 &&
                record.operation &&
                record.operation->name == "inc" &&
                lincheck::to_string(record).find("tx_id=") != std::string::npos &&
                lincheck::to_string(record).find("tx_depth=1") != std::string::npos;
        }),
        "STM switch-point trace events should retain transaction metadata"
    );
    require(
        std::any_of(result.event_dependencies.nodes.begin(), result.event_dependencies.nodes.end(), [](const auto& node) {
            return node.stream == "trace" &&
                node.kind == "switch-point" &&
                node.resource_id.rfind("tx#", 0) == 0;
        }),
        "STM trace dependency nodes should expose transaction resources"
    );
    require(!result.schedule.empty(), "STM-hook failure should expose the hook-driven schedule");
}

void model_checker_context_switch_bound_limits_exploration() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto sequential_only = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .max_context_switches_per_schedule(0)
        .check(spec);

    const auto with_switches = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .max_context_switches_per_schedule(2)
        .minimize_failed_scenario(false)
        .check(spec);

    require(sequential_only.success, "zero context-switch bound should only explore sequential counter runs");
    require(sequential_only.stats.scenarios_generated == 1, "successful model check should report generated scenarios");
    require(sequential_only.stats.schedules_explored > 0, "successful model check should report explored schedules");
    require(
        sequential_only.stats.max_context_switch_depth_explored == 0,
        "zero context-switch bound should not explore context-switching schedules"
    );
    require(
        sequential_only.stats.schedules_pruned_by_context_bound > 0,
        "zero context-switch bound should report context-bound pruning"
    );
    require(!with_switches.success, "larger context-switch bound should expose the broken counter");
    require(with_switches.failure == lincheck::FailureKind::invalid_results, "bounded interleaving failure should be invalid-results");
    require(with_switches.stats.schedules_explored > 0, "failing model check should report explored schedules");
    require(
        with_switches.stats.context_switch_depth_increases > 0,
        "model checker should increase context-switch depth after exhausting shallower frontier prefixes"
    );
    require(
        with_switches.stats.max_context_switch_depth_explored > 0,
        "larger context-switch bound should explore context-switching schedules"
    );
    require(
        with_switches.stats.schedules_generated >= with_switches.stats.schedules_explored,
        "generated schedule count should cover explored schedules"
    );
}

void model_checker_records_observed_frontier_choices() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(2)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "observed frontier should find the two-decision lost-update interleaving");
    require(result.failure == lincheck::FailureKind::invalid_results, "observed frontier failure should be invalid-results");
    require(!result.schedule.empty(), "observed frontier failure should expose the replay schedule");
    require(!result.schedule_decisions.empty(), "observed frontier should record scheduler decisions");
    require(
        result.schedule_decisions.size() <= result.schedule.size(),
        "recorded scheduler decisions should correspond to replay schedule choices"
    );
    for (std::size_t index = 0; index < result.schedule_decisions.size(); ++index) {
        require(
            result.schedule_decisions[index].switch_position == index,
            "scheduler decisions should expose stable switch-position indexes"
        );
        require(!result.schedule_decisions[index].location.empty(), "scheduler decisions should expose switch locations");
    }
    require(
        result.trace.find("schedule decisions:\n") != std::string::npos,
        "model-checker trace should include schedule decision metadata"
    );
    require(
        result.trace.find("runnable:") != std::string::npos,
        "model-checker trace should include runnable choices for schedule decisions"
    );
    require(
        result.trace.find(" operations:") != std::string::npos,
        "model-checker trace should include active operation contexts for schedule decisions"
    );

    const auto has_two_runnable_choices = std::any_of(
        result.schedule_decisions.begin(),
        result.schedule_decisions.end(),
        [](const lincheck::ScheduleDecision& decision) {
            return std::find(decision.runnable_threads.begin(), decision.runnable_threads.end(), 0) != decision.runnable_threads.end() &&
                std::find(decision.runnable_threads.begin(), decision.runnable_threads.end(), 1) != decision.runnable_threads.end();
        }
    );
    require(has_two_runnable_choices, "observed frontier should capture runnable alternatives at a scheduling decision");
    const auto has_runnable_operation = std::any_of(
        result.schedule_decisions.begin(),
        result.schedule_decisions.end(),
        [](const lincheck::ScheduleDecision& decision) {
            return std::any_of(
                decision.runnable_operations.begin(),
                decision.runnable_operations.end(),
                [](const lincheck::OperationContext& context) {
                    return context.thread_id >= 0 && context.name == "inc";
                }
            );
        }
    );
    require(has_runnable_operation, "observed frontier should capture runnable operation contexts");
    require(result.stats.schedules_generated > result.stats.schedules_explored, "frontier traversal should enqueue observed alternatives");
}

void model_checker_seed_changes_schedule_exploration_order() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    const lincheck::Actor inc{
        .operation_index = 0,
        .name = "inc",
        .group = "",
        .non_parallel = false,
        .one_shot = false,
        .exception_results = false,
        .arguments = {}
    };
    scenario.parallel = {
        {inc},
        {inc}
    };

    auto first_schedule_for_seed = [&](std::uint64_t seed) {
        const auto result = lincheck::ModelCheckingOptions()
            .iterations(1)
            .invocations_per_iteration(1)
            .max_schedule_length(3)
            .seed(seed)
            .check(spec, scenario);
        require(result.success, "seeded schedule-order probe should use a correct counter");
        require(!result.schedule.empty(), "seeded schedule-order probe should expose the explored schedule");
        return result.schedule;
    };

    const auto default_schedule = first_schedule_for_seed(0);
    std::uint64_t differing_seed = 0;
    std::vector<int> differing_schedule;
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        auto candidate = first_schedule_for_seed(seed);
        if (candidate != default_schedule) {
            differing_seed = seed;
            differing_schedule = std::move(candidate);
            break;
        }
    }

    require(differing_seed != 0, "a nonzero seed should be able to change model-checker schedule order");
    require(first_schedule_for_seed(differing_seed) == differing_schedule, "seeded model-checker schedule order should be deterministic");
    require(first_schedule_for_seed(0) == default_schedule, "seed zero should preserve the default schedule order");
}

void model_checker_seed_weights_same_depth_frontier_prefixes() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    const lincheck::Actor inc{
        .operation_index = 0,
        .name = "inc",
        .group = "",
        .non_parallel = false,
        .one_shot = false,
        .exception_results = false,
        .arguments = {}
    };
    scenario.parallel = {
        {inc},
        {inc},
        {inc}
    };

    auto second_explored_schedule = [&](std::uint64_t seed) {
        const auto result = lincheck::ModelCheckingOptions()
            .iterations(1)
            .invocations_per_iteration(2)
            .max_schedule_length(2)
            .seed(seed)
            .check(spec, scenario);
        require(result.success, "weighted-frontier probe should use a correct counter");
        require(result.stats.schedules_explored == 2, "weighted-frontier probe should explore exactly two prefixes");
        require(!result.schedule.empty(), "weighted-frontier probe should expose the last explored schedule");
        return result.schedule;
    };

    const auto default_schedule = second_explored_schedule(0);
    std::uint64_t differing_seed = 0;
    std::vector<int> differing_schedule;
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        auto candidate = second_explored_schedule(seed);
        if (candidate != default_schedule) {
            differing_seed = seed;
            differing_schedule = std::move(candidate);
            break;
        }
    }

    require(differing_seed != 0, "a nonzero seed should be able to weight same-depth frontier prefixes differently");
    require(second_explored_schedule(differing_seed) == differing_schedule, "same-depth frontier weighting should be deterministic for a seed");
    require(second_explored_schedule(0) == default_schedule, "seed zero should preserve FIFO same-depth frontier traversal");
}

void model_checker_reports_invocation_budget_pruning() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "budgeted model check should still accept the atomic counter");
    require(result.stats.schedules_explored == 1, "invocation budget should cap explored schedules");
    require(
        result.stats.schedules_pruned_by_invocation_budget == 1,
        "invocation budget should report the first skipped schedule"
    );
}

void model_checker_prunes_duplicate_successful_public_histories() {
    lincheck::TestSpec spec = lincheck::test<SwitchOnlyOperation, SequentialSwitchOnlyOperation>()
        .operation("ping", &SwitchOnlyOperation::ping, &SequentialSwitchOnlyOperation::ping);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "switch-only operations should be linearizable");
    require(result.stats.schedules_explored > 1, "duplicate-history probe should explore multiple schedules");
    require(
        result.stats.verifications_pruned_by_duplicate_history > 0,
        "model checker should skip verifier work for duplicate successful public histories"
    );
}

void model_checker_can_prune_explicitly_independent_operation_contexts() {
    auto independent = lincheck::independent_operation_group("readonly-ping");
    lincheck::TestSpec spec = lincheck::test<SwitchOnlyOperation, SequentialSwitchOnlyOperation>()
        .operation_with_options(
            "ping",
            &SwitchOnlyOperation::ping,
            &SequentialSwitchOnlyOperation::ping,
            independent
        );

    lincheck::RandomExecutionGenerator generator(
        spec,
        2,
        1,
        0,
        0,
        0
    );
    const auto scenario = generator.next();
    require(scenario.parallel.size() == 2, "independent-operation probe should generate two threads");

    const auto without_reduction = lincheck::ModelCheckingOptions()
        .iterations(1)
        .invocations_per_iteration(16)
        .max_schedule_length(4)
        .check(spec, scenario);

    require(without_reduction.success, "independent-operation probe should be linearizable without reduction");
    require(
        without_reduction.stats.schedules_pruned_by_operation_context == 0,
        "operation-context reduction should be disabled by default"
    );

    const auto with_reduction = lincheck::ModelCheckingOptions()
        .iterations(1)
        .invocations_per_iteration(16)
        .max_schedule_length(4)
        .operation_context_reduction()
        .check(spec, scenario);

    require(with_reduction.success, "independent-operation probe should remain linearizable with reduction");
    require(
        with_reduction.stats.schedules_pruned_by_operation_context > 0,
        "operation-context reduction should prune explicitly independent runnable operations"
    );
    require(
        std::any_of(
            with_reduction.schedule_decisions.begin(),
            with_reduction.schedule_decisions.end(),
            [](const lincheck::ScheduleDecision& decision) {
                return std::any_of(
                    decision.runnable_operations.begin(),
                    decision.runnable_operations.end(),
                    [](const lincheck::OperationContext& context) {
                        return context.independence_group == "readonly-ping";
                    }
                );
            }
        ),
        "schedule decisions should retain operation independence metadata"
    );
    require(
        with_reduction.trace.find("independent=readonly-ping") != std::string::npos,
        "schedule decision traces should include independence metadata"
    );
}

void model_checker_can_prune_disjoint_operation_dependency_footprints() {
    lincheck::TestSpec spec = lincheck::test<DisjointAtomicSlots, SequentialDisjointAtomicSlots>()
        .operation("touch_left", &DisjointAtomicSlots::touch_left, &SequentialDisjointAtomicSlots::touch_left)
        .operation("touch_right", &DisjointAtomicSlots::touch_right, &SequentialDisjointAtomicSlots::touch_right)
        .sequential_state([](const SequentialDisjointAtomicSlots& model) {
            return model.state_key();
        });

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "touch_left", .group = "", .non_parallel = false, .one_shot = false, .exception_results = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "touch_right", .group = "", .non_parallel = false, .one_shot = false, .exception_results = false, .arguments = {}}}
    };

    const auto without_reduction = lincheck::ModelCheckingOptions()
        .iterations(1)
        .invocations_per_iteration(16)
        .max_schedule_length(5)
        .check(spec, scenario);

    require(without_reduction.success, "disjoint-footprint probe should be linearizable without reduction");
    require(
        without_reduction.stats.schedules_pruned_by_event_dependency == 0,
        "event-dependency reduction should be disabled by default"
    );
    require(
        without_reduction.operation_dependency_footprints.size() == 2,
        "disjoint-footprint probe should retain one footprint per public operation"
    );

    const auto with_reduction = lincheck::ModelCheckingOptions()
        .iterations(1)
        .invocations_per_iteration(16)
        .max_schedule_length(5)
        .event_dependency_reduction()
        .check(spec, scenario);

    require(with_reduction.success, "disjoint-footprint probe should remain linearizable with footprint reduction");
    require(
        with_reduction.stats.schedules_pruned_by_event_dependency > 0,
        "event-dependency reduction should prune operations with disjoint observed resources"
    );
    require(
        std::any_of(
            with_reduction.operation_dependency_footprints.begin(),
            with_reduction.operation_dependency_footprints.end(),
            [](const lincheck::OperationDependencyFootprint& footprint) {
                return footprint.operation.name == "touch_left" &&
                    footprint.event_count >= 2 &&
                    std::find(footprint.streams.begin(), footprint.streams.end(), "memory") != footprint.streams.end() &&
                    std::any_of(footprint.resources.begin(), footprint.resources.end(), [](const std::string& resource) {
                        return resource.rfind("obj#", 0) == 0;
                    });
            }
        ),
        "event-dependency reduction should use retained public-operation memory footprints"
    );
}

void minimizer_removes_unneeded_init_and_post_actors() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(1)
        .actors_after(1)
        .max_schedule_length(3)
        .check(spec);

    require(!result.success, "model checker should still find broken counter with extra init/post actors");
    require(result.failure == lincheck::FailureKind::invalid_results, "minimized broken counter should remain invalid-results failure");
    require(result.scenario.init.empty(), "minimizer should remove unneeded init actors");
    require(result.scenario.post.empty(), "minimizer should remove unneeded post actors");
    require(result.scenario.parallel.size() == 2, "minimized broken counter should keep two parallel threads");
    require(result.scenario.parallel[0].size() == 1, "minimized broken counter should keep one actor in first thread");
    require(result.scenario.parallel[1].size() == 1, "minimized broken counter should keep one actor in second thread");
}

void model_checker_finds_broken_var_counter() {
    lincheck::TestSpec spec = lincheck::test<BrokenVarCounter, SequentialCounter>()
        .operation("inc", &BrokenVarCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(!result.success, "model checker should find broken var counter");
    require(result.failure == lincheck::FailureKind::invalid_results, "broken var counter should have invalid-results failure kind");
    require(result.trace.find("var.read") != std::string::npos, "var counter failure should include tracked var read");
}

void model_checker_finds_broken_var_operator_counter() {
    lincheck::TestSpec spec = lincheck::test<BrokenVarOperatorCounter, SequentialCounter>()
        .operation("inc", &BrokenVarOperatorCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(!result.success, "model checker should find broken var counter using operators");
    require(result.failure == lincheck::FailureKind::invalid_results, "operator var counter should have invalid-results failure kind");
    require(result.trace.find("var.read") != std::string::npos, "operator var counter failure should include tracked var read");
    require(result.trace.find("var.write") != std::string::npos, "operator var counter failure should include tracked var write");
}

void model_checker_finds_source_macro_counter() {
    lincheck::TestSpec spec = lincheck::test<SourceMacroCounter, SequentialCounter>()
        .operation("inc", &SourceMacroCounter::inc, &SequentialCounter::inc)
        .state_representation([](const SourceMacroCounter& counter) {
            return "value=" + std::to_string(counter.value);
        });

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(!result.success, "model checker should find source macro counter");
    require(result.failure == lincheck::FailureKind::invalid_results, "source macro counter should have invalid-results failure kind");
    require(result.state_representation.find("value=") != std::string::npos, "source macro failure should capture state representation");
    require(result.trace.find("source.read") != std::string::npos, "source macro failure should include tracked read");
    require(result.trace.find("source accesses:\n") != std::string::npos, "source macro failure should include structured source access section");
    require(
        has_source_access(result, lincheck::SourceAccessKind::read),
        "source macro failure should retain structured read access records"
    );
    require(
        has_source_access(result, lincheck::SourceAccessKind::write),
        "source macro failure should retain structured write access records"
    );
    require(result.trace.find("state:\n  value=") != std::string::npos, "source macro failure trace should include state representation");
    require(result.trace.find("lincheck_mvp_tests.cpp") != std::string::npos, "source macro trace should include source file");
}

void model_checker_accepts_atomic_counter() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "model checker should accept atomic fetch_add counter");
}

void test_builder_supports_custom_object_factories() {
    lincheck::TestSpec spec = lincheck::test<FactoryCounter, NonCopySequentialCounter>()
        .concurrent_factory([] {
            return std::make_unique<FactoryCounter>(0);
        })
        .sequential_factory([] {
            return std::make_shared<NonCopySequentialCounter>(0);
        })
        .sequential_cloner([](const NonCopySequentialCounter& model) {
            return std::make_unique<NonCopySequentialCounter>(model.value);
        })
        .operation("inc", &FactoryCounter::inc, &NonCopySequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "model checker should accept custom factories and sequential cloner");
}

void test_builder_supports_callable_operations() {
    lincheck::TestSpec spec = lincheck::test<AtomicAccumulator, SequentialAccumulator>()
        .operation_callable<int>(
            "add",
            [](AtomicAccumulator& counter, int delta) {
                return counter.add(delta);
            },
            [](SequentialAccumulator* model, int delta) {
                return model->add(delta);
            },
            lincheck::values<int>({1})
        );

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "model checker should accept callable operation registration");
}

void options_support_named_checks() {
    const auto model_result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check("named-broken-counter", [] {
            return lincheck::test<BrokenCounter, SequentialCounter>()
                .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc)
                .build();
        });

    require(!model_result.success, "named model check should still run the checker");
    require(model_result.test_name == "named-broken-counter", "named model check should record the test name");
    require(model_result.trace.rfind("test: named-broken-counter\n", 0) == 0, "named model check failure trace should include the test name prefix");

    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    const auto stress_result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .check("named-stress-counter", spec);

    require(stress_result.success, "named stress check should still run the checker");
    require(stress_result.test_name == "named-stress-counter", "named stress check should record the test name");
    require(stress_result.trace.empty(), "successful named stress check should not create a synthetic trace");
}

void options_reject_invalid_counts() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    require_invalid_argument(
        [] { lincheck::StressOptions().iterations(0); },
        "stress options should reject zero iterations",
        "iterations"
    );
    require_invalid_argument(
        [] { lincheck::StressOptions().invocations_per_iteration(0); },
        "stress options should reject zero invocations per iteration",
        "invocations_per_iteration"
    );
    require_invalid_argument(
        [] { lincheck::StressOptions().threads(0); },
        "stress options should reject zero threads",
        "threads"
    );
    require_invalid_argument(
        [] { lincheck::StressOptions().actors_before(-1); },
        "stress options should reject negative init actor counts",
        "actors_before"
    );
    require_invalid_argument(
        [] { lincheck::StressOptions().invocation_timeout(std::chrono::milliseconds(-1)); },
        "stress options should reject negative timeouts",
        "invocation_timeout"
    );
    require_invalid_argument(
        [] { lincheck::ModelCheckingOptions().max_schedule_length(0); },
        "model-checking options should reject zero schedule length",
        "max_schedule_length"
    );
    require_invalid_argument(
        [] { lincheck::ModelCheckingOptions().max_context_switches_per_schedule(-2); },
        "model-checking options should reject invalid context-switch bounds",
        "max_context_switches_per_schedule"
    );
    require_invalid_argument(
        [] { lincheck::ModelCheckingOptions().obstruction_switch_bound(0); },
        "model-checking options should reject zero obstruction switch bound",
        "obstruction_switch_bound"
    );
    require_invalid_argument(
        [] { lincheck::StressOptions().memory_model(lincheck::MemoryModel::cxx_release_acquire); },
        "stress options should reject unsupported memory models",
        "memory_model"
    );
    require_invalid_argument(
        [] { lincheck::ModelCheckingOptions().memory_model(lincheck::MemoryModel::cxx_relaxed); },
        "model-checking options should reject unsupported memory models",
        "sequentially consistent"
    );
    require_invalid_argument(
        [&] { lincheck::RandomExecutionGenerator(spec, 0, 1, 0, 0, 0); },
        "random generator should reject zero threads",
        "threads"
    );
    lincheck::StressOptions().memory_model(lincheck::MemoryModel::sequential_consistency);
    lincheck::ModelCheckingOptions().memory_model(lincheck::MemoryModel::sequential_consistency);
    require(
        std::string(lincheck::memory_model_name(lincheck::MemoryModel::sequential_consistency)) ==
            "sequential_consistency",
        "memory model names should be stable for diagnostics"
    );
}

void model_checker_reports_non_seq_cst_memory_order_warning_on_success() {
    lincheck::TestSpec spec = lincheck::test<RelaxedAtomicCounter, SequentialCounter>()
        .operation("inc", &RelaxedAtomicCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "relaxed fetch_add counter should still be linearizable under the SC wrapper model");
    require(
        has_warning(result, "atomic.fetch_add", "sequentially consistent"),
        "model checker should return non-seq_cst atomic warning on success"
    );
    require(
        has_memory_event(result, lincheck::MemoryEventKind::atomic_fetch_add),
        "model checker should expose structured memory events on CheckResult"
    );
    require(
        std::any_of(result.memory_events.begin(), result.memory_events.end(), [](const auto& record) {
            return record.event.kind == lincheck::MemoryEventKind::atomic_fetch_add && record.thread_id >= 0;
        }),
        "model-checker memory events should include worker thread IDs"
    );
    require(
        has_memory_event_object_id(result, lincheck::MemoryEventKind::atomic_fetch_add),
        "model-checker memory events should retain stable object IDs"
    );
}

void model_checker_accepts_custom_value_results() {
    lincheck::TestSpec spec = lincheck::test<CustomResultAtomicCounter, CustomResultSequentialCounter>()
        .operation("inc", &CustomResultAtomicCounter::inc, &CustomResultSequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check(spec);

    require(result.success, "model checker should accept custom user-type operation results");
}

void successful_checks_return_clock_warnings() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    auto has_rdtsc_warning = [](const lincheck::CheckResult& result) {
        return std::any_of(result.warnings.begin(), result.warnings.end(), [](const std::string& warning) {
            return warning.find("rdtsc") != std::string::npos || warning.find("rdtscp") != std::string::npos;
        });
    };

    const auto stress = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .clock_source(lincheck::ClockSourceKind::rdtsc)
        .check(spec);

    const auto model = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .max_schedule_length(1)
        .clock_source(lincheck::ClockSourceKind::rdtsc)
        .check(spec);

    require(stress.success, "stress check should still pass with rdtsc clock");
    require(model.success, "model check should still pass with rdtsc clock");
    require(has_rdtsc_warning(stress), "successful stress check should return rdtsc warning");
    require(has_rdtsc_warning(model), "successful model check should return rdtsc warning");
}

void atomic_clock_source_is_global_across_instances() {
    lincheck::AtomicClockSource first;
    lincheck::AtomicClockSource second;

    const auto a = first.now();
    const auto b = second.now();
    const auto c = first.now();

    require(b == a + 1, "separate atomic clock sources should share one strict sequence");
    require(c == b + 1, "atomic clock source should remain globally monotonic");
}

void failure_trace_includes_clock_warnings() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .clock_source(lincheck::ClockSourceKind::rdtsc)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "rdtsc broken counter should fail");
    require(!result.warnings.empty(), "rdtsc failure should return warnings");
    require(result.trace.find("warnings:\n") != std::string::npos, "failure trace should include warnings section");
    require(result.trace.find("rdtsc") != std::string::npos || result.trace.find("rdtscp") != std::string::npos, "failure trace should include rdtsc warning text");
}

void model_checker_trace_filters_runtime_lines_without_changing_execution() {
    lincheck::TestSpec spec = lincheck::test<BrokenCounter, SequentialCounter>()
        .operation("inc", &BrokenCounter::inc, &SequentialCounter::inc);

    const auto excluded = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .minimize_failed_scenario(false)
        .trace_exclude("atomic.load")
        .check(spec);

    require(!excluded.success, "model checker should still find a bug when trace lines are excluded");
    require(excluded.failure == lincheck::FailureKind::invalid_results, "filtered model-checker failure should keep failure kind");
    require(excluded.trace.find("schedule:") != std::string::npos, "filtered trace should still include schedule metadata");
    require(excluded.trace.find("atomic.load") == std::string::npos, "excluded trace pattern should be suppressed");
    require(excluded.trace.find("atomic.store") != std::string::npos, "non-excluded runtime lines should remain");

    const auto included = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .minimize_failed_scenario(false)
        .trace_include("operation.start")
        .check(spec);

    require(!included.success, "model checker should still find a bug when trace lines are included selectively");
    require(included.trace.find("operation.start") != std::string::npos, "included trace pattern should be retained");
    require(included.trace.find("operation.finish") == std::string::npos, "non-included runtime lines should be suppressed");
}

void atomic_wrapper_supports_more_operations_and_memory_order_traces() {
    RecordingRuntime runtime;
    lincheck::atomic<int> value{10};
    {
        lincheck::ScopedRuntime scoped(&runtime);
        require(value.load(std::memory_order_relaxed) == 10, "atomic load should return current value");
        require(value.exchange(20, std::memory_order_acq_rel) == 10, "atomic exchange should return old value");
        require(value.fetch_sub(3, std::memory_order_release) == 20, "atomic fetch_sub should return old value");
        require(value.fetch_or(0x20, std::memory_order_acquire) == 17, "atomic fetch_or should return old value");
        require(value.fetch_and(0x2f, std::memory_order_relaxed) == 49, "atomic fetch_and should return old value");
        require(value.fetch_xor(0x02, std::memory_order_seq_cst) == 33, "atomic fetch_xor should return old value");
        int expected = 35;
        require(
            value.compare_exchange_strong(expected, 30, std::memory_order_acq_rel, std::memory_order_acquire),
            "strong CAS should succeed"
        );
        expected = 17;
        require(
            !value.compare_exchange_weak(expected, 40, std::memory_order_acq_rel, std::memory_order_relaxed),
            "weak CAS should fail when expected mismatches"
        );
        require(expected == 30, "failed CAS should update expected");
        expected = 30;
        require(
            value.compare_exchange_strong(expected, 31, std::memory_order_release),
            "single-order strong CAS should succeed"
        );
        expected = 100;
        require(
            !value.compare_exchange_weak(expected, 32, std::memory_order_acq_rel),
            "single-order weak CAS should fail when expected mismatches"
        );
        require(expected == 31, "single-order failed CAS should update expected");
        require(static_cast<int>(value) == 31, "atomic conversion should load the current value");
        require((value = 5) == 5, "atomic assignment should store and return the assigned value");
        require(++value == 6, "prefix increment should return the incremented value");
        require(value++ == 6, "postfix increment should return the old value");
        require(--value == 6, "prefix decrement should return the decremented value");
        require(value-- == 6, "postfix decrement should return the old value");
        require((value += 4) == 9, "compound add should return the new value");
        require((value -= 3) == 6, "compound subtract should return the new value");
        require((value |= 0x08) == 14, "compound or should return the new value");
        require((value &= 0x0f) == 14, "compound and should return the new value");
        require((value ^= 0x03) == 13, "compound xor should return the new value");
        value.wait(12, std::memory_order_acquire);
        value.notify_one();
        value.notify_all();
    }

    auto has_event = [&](const std::string& needle) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };

    require(has_event("atomic.load order=relaxed"), "atomic load trace should include relaxed order");
    require(has_event("atomic.load order=seq_cst"), "atomic conversion trace should include seq_cst load");
    require(has_event("atomic.store order=seq_cst 5"), "atomic assignment trace should include seq_cst store");
    require(has_event("atomic.exchange order=acq_rel"), "atomic exchange trace should include acq_rel order");
    require(has_event("atomic.fetch_sub order=release"), "atomic fetch_sub trace should include release order");
    require(has_event("atomic.fetch_or order=acquire"), "atomic fetch_or trace should include acquire order");
    require(has_event("atomic.fetch_and order=relaxed"), "atomic fetch_and trace should include relaxed order");
    require(has_event("atomic.fetch_xor order=seq_cst"), "atomic fetch_xor trace should include seq_cst order");
    require(has_event("atomic.compare_exchange_strong success_order=acq_rel failure_order=acquire"), "strong CAS trace should include success/failure orders");
    require(has_event("atomic.compare_exchange_weak success_order=acq_rel failure_order=relaxed"), "weak CAS trace should include success/failure orders");
    require(has_event("atomic.compare_exchange_strong success_order=release failure_order=relaxed"), "single-order release CAS should derive a relaxed failure order");
    require(has_event("atomic.compare_exchange_weak success_order=acq_rel failure_order=acquire"), "single-order acq_rel CAS should derive an acquire failure order");
    require(has_event("atomic.wait order=acquire expected=12"), "atomic wait trace should include order and expected value");
    require(has_event("atomic.notify_one object=obj#"), "atomic notify_one trace should include stable object ID");
    require(has_event("atomic.notify_all object=obj#"), "atomic notify_all trace should include stable object ID");
    require(
        has_warning(runtime.warnings, "atomic.load", "sequentially consistent"),
        "atomic load should warn when a non-seq_cst order is observed"
    );
    require(
        has_warning(runtime.warnings, "atomic.wait", "acquire"),
        "atomic wait should warn when a non-seq_cst order is observed"
    );
    require(
        has_warning(runtime.warnings, "atomic.compare_exchange_weak failure", "relaxed"),
        "CAS failure order should warn when it is non-seq_cst"
    );
}

void atomic_wrapper_rejects_invalid_memory_orders_before_std_atomic_calls() {
    lincheck::atomic<int> value{0};

    require_invalid_argument(
        [&] { (void)value.load(std::memory_order_release); },
        "atomic load should reject release order",
        "atomic.load"
    );
    require_invalid_argument(
        [&] { value.store(1, std::memory_order_acquire); },
        "atomic store should reject acquire order",
        "atomic.store"
    );
    require_invalid_argument(
        [&] { value.wait(0, std::memory_order_acq_rel); },
        "atomic wait should reject acq_rel order",
        "atomic.wait"
    );

    int expected = 0;
    require_invalid_argument(
        [&] { (void)value.compare_exchange_strong(expected, 1, std::memory_order_seq_cst, std::memory_order_release); },
        "strong CAS should reject release failure order",
        "failure"
    );
    expected = 0;
    require_invalid_argument(
        [&] { (void)value.compare_exchange_weak(expected, 1, std::memory_order_relaxed, std::memory_order_acquire); },
        "weak CAS should reject failure order stronger than success order",
        "stronger"
    );
    require_invalid_argument(
        [&] { (void)value.exchange(1, static_cast<std::memory_order>(99)); },
        "atomic exchange should reject unknown order",
        "unknown"
    );
    require_invalid_argument(
        [&] { lincheck::atomic_thread_fence(static_cast<std::memory_order>(99)); },
        "atomic_thread_fence should reject unknown order",
        "unknown"
    );
}

void atomic_and_fence_wrappers_emit_structured_memory_events() {
    RecordingRuntime runtime;
    lincheck::atomic<int> value{1};
    {
        lincheck::ScopedRuntime scoped(&runtime);
        require(value.load(std::memory_order_relaxed) == 1, "structured load probe should read initial value");
        value.store(2, std::memory_order_release);
        require(value.exchange(3, std::memory_order_acq_rel) == 2, "structured exchange probe should return old value");
        require(value.fetch_add(4, std::memory_order_acquire) == 3, "structured fetch_add probe should return old value");
        int expected = 99;
        require(
            !value.compare_exchange_strong(expected, 10, std::memory_order_acq_rel, std::memory_order_acquire),
            "structured CAS probe should fail"
        );
        require(expected == 7, "structured CAS probe should update expected");
        value.wait(100, std::memory_order_acquire);
        value.notify_one();
        value.notify_all();
        lincheck::atomic_thread_fence(std::memory_order_release);
        lincheck::atomic_signal_fence(std::memory_order_acquire);
    }

    auto find_event = [&](lincheck::MemoryEventKind kind) -> const lincheck::MemoryEvent* {
        const auto it = std::find_if(runtime.memory_events.begin(), runtime.memory_events.end(), [&](const auto& event) {
            return event.kind == kind;
        });
        return it == runtime.memory_events.end() ? nullptr : &*it;
    };

    const auto* load = find_event(lincheck::MemoryEventKind::atomic_load);
    require(load != nullptr, "structured memory events should include atomic load");
    require(load->object == &value, "structured load event should identify the wrapper object");
    require(load->success_order == std::memory_order_relaxed, "structured load event should record memory order");
    require(load->has_observed && lincheck::value_cast<int>(load->observed) == 1, "structured load event should record observed value");

    const auto* store = find_event(lincheck::MemoryEventKind::atomic_store);
    require(store != nullptr, "structured memory events should include atomic store");
    require(store->has_operand && lincheck::value_cast<int>(store->operand) == 2, "structured store event should record stored value");

    const auto* exchange = find_event(lincheck::MemoryEventKind::atomic_exchange);
    require(exchange != nullptr, "structured memory events should include atomic exchange");
    require(exchange->has_operand && lincheck::value_cast<int>(exchange->operand) == 3, "structured exchange event should record desired value");
    require(exchange->has_observed && lincheck::value_cast<int>(exchange->observed) == 2, "structured exchange event should record observed value");

    const auto* fetch_add = find_event(lincheck::MemoryEventKind::atomic_fetch_add);
    require(fetch_add != nullptr, "structured memory events should include atomic fetch_add");
    require(fetch_add->success_order == std::memory_order_acquire, "structured fetch_add event should record memory order");
    require(fetch_add->has_operand && lincheck::value_cast<int>(fetch_add->operand) == 4, "structured fetch_add event should record operand");
    require(fetch_add->has_observed && lincheck::value_cast<int>(fetch_add->observed) == 3, "structured fetch_add event should record old value");

    const auto* cas = find_event(lincheck::MemoryEventKind::atomic_compare_exchange_strong);
    require(cas != nullptr, "structured memory events should include CAS");
    require(cas->has_failure_order && cas->failure_order == std::memory_order_acquire, "structured CAS event should record failure order");
    require(cas->has_expected && lincheck::value_cast<int>(cas->expected) == 99, "structured CAS event should record expected input");
    require(cas->has_operand && lincheck::value_cast<int>(cas->operand) == 10, "structured CAS event should record desired value");
    require(cas->has_observed && lincheck::value_cast<int>(cas->observed) == 7, "structured CAS event should record observed value");
    require(cas->has_success && !cas->success, "structured CAS event should record success flag");

    const auto* wait = find_event(lincheck::MemoryEventKind::atomic_wait);
    require(wait != nullptr, "structured memory events should include atomic wait");
    require(wait->has_expected && lincheck::value_cast<int>(wait->expected) == 100, "structured wait event should record expected value");

    require(find_event(lincheck::MemoryEventKind::atomic_notify_one) != nullptr, "structured memory events should include notify_one");
    require(find_event(lincheck::MemoryEventKind::atomic_notify_all) != nullptr, "structured memory events should include notify_all");

    const auto* thread_fence = find_event(lincheck::MemoryEventKind::atomic_thread_fence);
    require(thread_fence != nullptr, "structured memory events should include thread fences");
    require(thread_fence->success_order == std::memory_order_release, "structured thread-fence event should record memory order");
    require(thread_fence->object == nullptr, "structured thread-fence event should not claim an atomic object");

    const auto* signal_fence = find_event(lincheck::MemoryEventKind::atomic_signal_fence);
    require(signal_fence != nullptr, "structured memory events should include signal fences");
    require(signal_fence->success_order == std::memory_order_acquire, "structured signal-fence event should record memory order");
    require(
        lincheck::to_string(*cas).find("atomic.compare_exchange_strong") != std::string::npos,
        "structured memory event formatting should name the operation"
    );
}

void atomic_ref_wrapper_emits_structured_memory_events_for_referenced_object() {
    RecordingRuntime runtime;
    int value = 1;
    lincheck::atomic_ref<int> ref(value);

    {
        lincheck::ScopedRuntime scoped(&runtime);
        require(ref.load(std::memory_order_relaxed) == 1, "atomic_ref load should read the referenced object");
        ref.store(2, std::memory_order_release);
        require(ref.fetch_add(3, std::memory_order_acquire) == 2, "atomic_ref fetch_add should return the old value");
        int expected = 5;
        require(
            ref.compare_exchange_strong(expected, 9, std::memory_order_acq_rel, std::memory_order_acquire),
            "atomic_ref CAS should update the referenced object"
        );
        ref.wait(100, std::memory_order_acquire);
        ref.notify_all();
    }

    require(value == 9, "atomic_ref operations should update the referenced object");

    auto find_event = [&](lincheck::MemoryEventKind kind) -> const lincheck::MemoryEvent* {
        const auto it = std::find_if(runtime.memory_events.begin(), runtime.memory_events.end(), [&](const auto& event) {
            return event.kind == kind;
        });
        return it == runtime.memory_events.end() ? nullptr : &*it;
    };

    const auto* load = find_event(lincheck::MemoryEventKind::atomic_load);
    require(load != nullptr, "atomic_ref should emit a structured load event");
    require(load->object == &value, "atomic_ref load event should identify the referenced object");
    require(load->success_order == std::memory_order_relaxed, "atomic_ref load event should record memory order");
    require(load->has_observed && lincheck::value_cast<int>(load->observed) == 1, "atomic_ref load should record observed value");

    const auto* store = find_event(lincheck::MemoryEventKind::atomic_store);
    require(store != nullptr, "atomic_ref should emit a structured store event");
    require(store->object == &value, "atomic_ref store event should identify the referenced object");
    require(store->has_operand && lincheck::value_cast<int>(store->operand) == 2, "atomic_ref store should record stored value");

    const auto* fetch_add = find_event(lincheck::MemoryEventKind::atomic_fetch_add);
    require(fetch_add != nullptr, "atomic_ref should emit a structured fetch_add event");
    require(fetch_add->object == &value, "atomic_ref fetch_add event should identify the referenced object");
    require(fetch_add->has_operand && lincheck::value_cast<int>(fetch_add->operand) == 3, "atomic_ref fetch_add should record operand");
    require(fetch_add->has_observed && lincheck::value_cast<int>(fetch_add->observed) == 2, "atomic_ref fetch_add should record old value");

    const auto* cas = find_event(lincheck::MemoryEventKind::atomic_compare_exchange_strong);
    require(cas != nullptr, "atomic_ref should emit a structured CAS event");
    require(cas->object == &value, "atomic_ref CAS event should identify the referenced object");
    require(cas->has_success && cas->success, "atomic_ref CAS should record success");

    const auto* wait = find_event(lincheck::MemoryEventKind::atomic_wait);
    require(wait != nullptr, "atomic_ref should emit a structured wait event");
    require(wait->object == &value, "atomic_ref wait event should identify the referenced object");
    require(wait->has_expected && lincheck::value_cast<int>(wait->expected) == 100, "atomic_ref wait should record expected value");
    require(
        find_event(lincheck::MemoryEventKind::atomic_notify_all) != nullptr,
        "atomic_ref should emit notify_all memory events"
    );
}

void runtime_wrappers_emit_stable_object_ids() {
    RecordingRuntime runtime;
    lincheck::atomic<int> atomic_value{1};
    lincheck::var<int> plain_value{2};
    lincheck::mutex mutex;
    lincheck::condition_variable condition;
    lincheck::parker parker;

    {
        lincheck::ScopedRuntime scoped(&runtime);
        require(atomic_value.load() == 1, "atomic load should return the stored value");
        atomic_value.store(3);
        atomic_value.wait(2);
        atomic_value.notify_one();
        atomic_value.notify_all();
        require(plain_value.get() == 2, "var get should return the stored value");
        plain_value.set(4);
        mutex.lock();
        mutex.unlock();
        require(mutex.try_lock(), "mutex try_lock should acquire an unlocked mutex");
        mutex.unlock();
        {
            lincheck::unique_lock lock(mutex);
            const auto status = condition.wait_for(lock, std::chrono::milliseconds(0));
            require(status == std::cv_status::timeout, "condition wait_for should time out with no notifier");
        }
        condition.notify_one();
        parker.unpark();
        parker.park();
    }

    auto has_event = [&](const std::string& first, const std::string& second) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(first) != std::string::npos && event.find(second) != std::string::npos;
        });
    };
    const auto has_raw_object = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("object=0x") != std::string::npos || event.find("lock=0x") != std::string::npos;
    });

    require(has_event("atomic.load", "object=obj#"), "atomic traces should include stable object IDs");
    require(has_event("atomic.store", "object=obj#"), "atomic store traces should include stable object IDs");
    require(has_event("atomic.wait", "object=obj#"), "atomic wait traces should include stable object IDs");
    require(has_event("atomic.notify_all", "object=obj#"), "atomic notify traces should include stable object IDs");
    require(has_event("var.read", "object=obj#"), "var read traces should include stable object IDs");
    require(has_event("var.write", "object=obj#"), "var write traces should include stable object IDs");
    require(has_event("mutex.lock", "object=obj#"), "mutex lock traces should include stable object IDs");
    require(has_event("mutex.try_lock", "object=obj#"), "mutex try_lock traces should include stable object IDs");
    require(has_event("condition_variable.wait_for", "lock=obj#"), "condition waits should include stable lock IDs");
    require(has_event("condition_variable.notify_one", "object=obj#"), "condition notify traces should include stable object IDs");
    require(has_event("parker.park", "object=obj#"), "parker park traces should include stable object IDs");
    require(has_event("parker.unpark", "object=obj#"), "parker unpark traces should include stable object IDs");
    require(!has_raw_object, "runtime wrapper traces should not expose raw object addresses");

    auto has_sync = [&](lincheck::SynchronizationEventKind kind) {
        return std::any_of(runtime.synchronization_events.begin(), runtime.synchronization_events.end(), [&](const auto& event) {
            return event.kind == kind && event.object != nullptr;
        });
    };
    require(has_sync(lincheck::SynchronizationEventKind::mutex_lock_attempt), "structured sync events should include mutex lock attempts");
    require(has_sync(lincheck::SynchronizationEventKind::mutex_lock_acquired), "structured sync events should include mutex acquisitions");
    require(has_sync(lincheck::SynchronizationEventKind::mutex_unlock), "structured sync events should include mutex unlocks");
    require(
        std::any_of(runtime.synchronization_events.begin(), runtime.synchronization_events.end(), [](const auto& event) {
            return event.kind == lincheck::SynchronizationEventKind::mutex_try_lock &&
                event.has_success &&
                event.success;
        }),
        "structured sync events should include mutex try_lock success"
    );
    require(
        std::any_of(runtime.synchronization_events.begin(), runtime.synchronization_events.end(), [](const auto& event) {
            return event.kind == lincheck::SynchronizationEventKind::condition_wait &&
                event.object != nullptr &&
                event.related_object != nullptr;
        }),
        "structured sync events should include condition waits with related lock"
    );
    require(has_sync(lincheck::SynchronizationEventKind::condition_wake), "structured sync events should include condition wakeups");
    require(has_sync(lincheck::SynchronizationEventKind::condition_notify_one), "structured sync events should include condition notifications");
    require(has_sync(lincheck::SynchronizationEventKind::atomic_wait), "structured sync events should include atomic waits");
    require(has_sync(lincheck::SynchronizationEventKind::atomic_wake), "structured sync events should include atomic wakeups");
    require(has_sync(lincheck::SynchronizationEventKind::atomic_notify_one), "structured sync events should include atomic notify_one");
    require(has_sync(lincheck::SynchronizationEventKind::atomic_notify_all), "structured sync events should include atomic notify_all");
    require(has_sync(lincheck::SynchronizationEventKind::parker_park), "structured sync events should include parker park");
    require(has_sync(lincheck::SynchronizationEventKind::parker_unpark), "structured sync events should include parker unpark");
}

void fence_helpers_emit_memory_order_and_location_traces() {
    RecordingRuntime runtime;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::atomic_thread_fence(std::memory_order_release);
        lincheck::atomic_signal_fence(std::memory_order_acquire);
        LC_THREAD_FENCE(std::memory_order_seq_cst);
        LC_SIGNAL_FENCE(std::memory_order_relaxed);
    }

    auto has_event = [&](const std::string& first, const std::string& second = "") {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(first) != std::string::npos &&
                (second.empty() || event.find(second) != std::string::npos);
        });
    };
    auto has_switch = [&](const std::string& needle) {
        return std::any_of(runtime.switches.begin(), runtime.switches.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };

    require(has_event("atomic_thread_fence order=release"), "thread fence trace should include release order");
    require(has_event("atomic_signal_fence order=acquire"), "signal fence trace should include acquire order");
    require(has_event("atomic_thread_fence order=seq_cst", "lincheck_mvp_tests.cpp"), "LC_THREAD_FENCE should include source file");
    require(has_event("atomic_signal_fence order=relaxed", "lincheck_mvp_tests.cpp"), "LC_SIGNAL_FENCE should include source file");
    require(has_switch("atomic_thread_fence"), "thread fence should emit switch point");
    require(has_switch("atomic_signal_fence"), "signal fence should emit switch point");
    require(
        has_warning(runtime.warnings, "atomic_thread_fence", "release"),
        "thread fence should warn for non-seq_cst order"
    );
    require(
        has_warning(runtime.warnings, "atomic_signal_fence", "relaxed"),
        "signal fence should warn for non-seq_cst source-location order"
    );
    require(
        std::any_of(runtime.memory_events.begin(), runtime.memory_events.end(), [](const auto& event) {
            return event.kind == lincheck::MemoryEventKind::atomic_thread_fence &&
                event.has_source &&
                lincheck::to_string(event).find("lincheck_mvp_tests.cpp") != std::string::npos &&
                lincheck::to_string(event).find("location=loc#") != std::string::npos;
        }),
        "source-location thread fences should carry structured memory event source metadata"
    );
    require(
        std::any_of(runtime.memory_events.begin(), runtime.memory_events.end(), [](const auto& event) {
            return event.kind == lincheck::MemoryEventKind::atomic_signal_fence &&
                event.has_source &&
                lincheck::to_string(event).find("lincheck_mvp_tests.cpp") != std::string::npos;
        }),
        "source-location signal fences should carry structured memory event source metadata"
    );
}

void obstruction_freedom_accepts_operation_that_completes_in_isolation() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(3)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    require(result.success, "obstruction-freedom check should accept operation that completes in isolation");
}

void obstruction_freedom_accepts_expected_operation_exception_results() {
    lincheck::TestSpec spec = lincheck::test<ThrowingPopQueue, ThrowingPopSequentialQueue>()
        .concurrent_factory([] {
            return std::make_shared<ThrowingPopQueue>(1);
        })
        .sequential_factory([] {
            return std::make_shared<ThrowingPopSequentialQueue>(1);
        })
        .operation_with_options(
            "pop",
            &ThrowingPopQueue::pop,
            &ThrowingPopSequentialQueue::pop,
            lincheck::exceptions_as_results()
        );

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(1)
        .actors_after(0)
        .max_schedule_length(2)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "obstruction-freedom check should compare opted-in operation exceptions as results");
}

void obstruction_freedom_rejects_operation_that_spins_in_isolation() {
    lincheck::TestSpec spec = lincheck::test<ObstructionSpin, SequentialCounter>()
        .operation("spin", &ObstructionSpin::spin, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check_obstruction_freedom()
        .obstruction_switch_bound(3)
        .check(spec);

    require(!result.success, "obstruction-freedom check should reject operation that spins in isolation");
    require(result.failure == lincheck::FailureKind::obstruction_freedom, "spin failure should have obstruction-freedom failure kind");
    require(result.message == "obstruction freedom violation", "spin failure should be reported as obstruction freedom");
    require(result.trace.find("obstruction freedom switch budget exceeded") != std::string::npos, "spin failure should explain budget exhaustion");
    require(result.trace.find("obstruction.spin") != std::string::npos, "spin failure should include switch trace");
    require(result.trace.find("trace events:\n") != std::string::npos, "spin failure should include structured trace events");
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.kind == "switch-point" &&
                record.thread_id == 0 &&
                record.operation &&
                record.operation->name == "spin" &&
                lincheck::to_string(record).find("operation=spin@0#0") != std::string::npos;
        }),
        "spin failure should retain obstruction trace events with operation context"
    );
    require(
        std::any_of(result.event_dependencies.nodes.begin(), result.event_dependencies.nodes.end(), [](const auto& node) {
            return node.stream == "trace" && node.kind == "switch-point";
        }),
        "spin failure should expose trace dependency nodes"
    );
}

void obstruction_freedom_rejects_operation_that_parks() {
    lincheck::TestSpec spec = lincheck::test<ObstructionPark, SequentialCounter>()
        .operation("wait", &ObstructionPark::wait, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    require(!result.success, "obstruction-freedom check should reject a parking operation");
    require(result.failure == lincheck::FailureKind::obstruction_freedom, "parking operation should fail obstruction freedom");
    require(result.trace.find("blocked on park") != std::string::npos, "obstruction trace should include park blocking reason");
}

void obstruction_freedom_rejects_operation_that_atomic_waits() {
    lincheck::TestSpec spec = lincheck::test<ObstructionAtomicWait, SequentialCounter>()
        .operation("wait", &ObstructionAtomicWait::wait, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    require(!result.success, "obstruction-freedom check should reject an atomic wait operation");
    require(result.failure == lincheck::FailureKind::obstruction_freedom, "atomic wait operation should fail obstruction freedom");
    require(result.trace.find("blocked on atomic wait") != std::string::npos, "obstruction trace should include atomic wait blocking reason");
}

void obstruction_freedom_rejects_operation_that_acquires_unavailable_semaphore() {
    lincheck::TestSpec spec = lincheck::test<ObstructionSemaphoreWait, SequentialCounter>()
        .operation("wait", &ObstructionSemaphoreWait::wait, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    require(!result.success, "obstruction-freedom check should reject an unavailable semaphore acquire");
    require(result.failure == lincheck::FailureKind::obstruction_freedom, "semaphore acquire should fail obstruction freedom");
    require(result.trace.find("blocked on semaphore") != std::string::npos, "obstruction trace should include semaphore blocking reason");
}

void obstruction_freedom_rejects_operation_that_waits_on_closed_latch() {
    lincheck::TestSpec spec = lincheck::test<ObstructionLatchWait, SequentialCounter>()
        .operation("wait", &ObstructionLatchWait::wait, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    require(!result.success, "obstruction-freedom check should reject a closed latch wait");
    require(result.failure == lincheck::FailureKind::obstruction_freedom, "latch wait should fail obstruction freedom");
    require(result.trace.find("blocked on latch") != std::string::npos, "obstruction trace should include latch blocking reason");
}

void obstruction_freedom_rejects_operation_that_waits_on_incomplete_barrier() {
    lincheck::TestSpec spec = lincheck::test<ObstructionBarrierWait, SequentialCounter>()
        .operation("wait", &ObstructionBarrierWait::wait, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check_obstruction_freedom()
        .obstruction_switch_bound(10)
        .check(spec);

    require(!result.success, "obstruction-freedom check should reject an incomplete barrier wait");
    require(result.failure == lincheck::FailureKind::obstruction_freedom, "barrier wait should fail obstruction freedom");
    require(result.trace.find("blocked on barrier") != std::string::npos, "obstruction trace should include barrier blocking reason");
}

void model_checker_reports_livelock_when_switch_budget_is_exceeded() {
    lincheck::TestSpec spec = lincheck::test<ObstructionSpin, SequentialCounter>()
        .operation("spin", &ObstructionSpin::spin, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .max_switch_points_per_schedule(3)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "model checker should report livelock for a cooperative infinite loop");
    require(result.failure == lincheck::FailureKind::livelock, "switch budget failure should have livelock failure kind");
    require(result.message == "livelock", "switch budget failure should preserve livelock message");
    require(result.trace.find("livelock: switch-point budget exceeded") != std::string::npos, "livelock trace should explain switch budget exhaustion");
    require(result.trace.find("obstruction.spin") != std::string::npos, "livelock trace should include the spinning switch point");
}

void verifier_rejects_histories_that_violate_real_time_order() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    lincheck::ExecutionResult overlapping;
    overlapping.parallel_results = {{lincheck::Value(2)}, {lincheck::Value(1)}};
    overlapping.parallel_intervals = {
        {lincheck::OperationInterval{.thread_id = 0, .actor_index = 0, .invocation_clock = 1, .response_clock = 4, .result = lincheck::Value(2)}},
        {lincheck::OperationInterval{.thread_id = 1, .actor_index = 0, .invocation_clock = 2, .response_clock = 3, .result = lincheck::Value(1)}}
    };

    lincheck::LinearizabilityVerifier verifier(spec);
    require(verifier.verify(scenario, overlapping), "overlapping operations may linearize in either order");

    lincheck::ExecutionResult non_overlapping = overlapping;
    non_overlapping.parallel_intervals = {
        {lincheck::OperationInterval{.thread_id = 0, .actor_index = 0, .invocation_clock = 1, .response_clock = 2, .result = lincheck::Value(2)}},
        {lincheck::OperationInterval{.thread_id = 1, .actor_index = 0, .invocation_clock = 3, .response_clock = 4, .result = lincheck::Value(1)}}
    };

    require(!verifier.verify(scenario, non_overlapping), "non-overlapping operations must respect real-time order");
}

void verifier_backtracks_parallel_orders_when_post_state_rejects_first_match() {
    lincheck::TestSpec spec = lincheck::test<AppendLog, AppendLog>()
        .operation("append", &AppendLog::append, &AppendLog::append, lincheck::strings({"A", "B"}))
        .operation("snapshot", &AppendLog::snapshot, &AppendLog::snapshot)
        .sequential_state([](const AppendLog& model) {
            return model.value;
        });

    lincheck::Actor append_a{
        .operation_index = 0,
        .name = "append",
        .group = "",
        .non_parallel = false,
        .one_shot = false,
        .arguments = {lincheck::Value(std::string("A"))}
    };
    lincheck::Actor append_b = append_a;
    append_b.arguments = {lincheck::Value(std::string("B"))};
    lincheck::Actor snapshot{
        .operation_index = 1,
        .name = "snapshot",
        .group = "",
        .non_parallel = false,
        .one_shot = false,
        .arguments = {}
    };

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {{append_a}, {append_b}};
    scenario.post = {snapshot};

    lincheck::ExecutionResult result;
    result.parallel_results = {{lincheck::Value{}}, {lincheck::Value{}}};
    result.post_results = {lincheck::Value(std::string("BA"))};

    lincheck::LinearizabilityVerifier verifier(spec);
    const auto report = verifier.verify_with_report(scenario, result);
    require(report.success, "verifier should try another parallel order when the first match fails post-state");
    require(verifier.verify(scenario, result), "plain verifier should also accept the post-compatible order");
}

void verifier_uses_optional_sequential_state_cache() {
    int state_key_calls = 0;
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc)
        .sequential_state([&](const SequentialCounter& model) {
            ++state_key_calls;
            return model.value;
        });

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {
            lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}},
            lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}
        },
        {
            lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}},
            lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}
        }
    };

    lincheck::ExecutionResult impossible;
    impossible.parallel_results = {
        {lincheck::Value(1), lincheck::Value(1)},
        {lincheck::Value(1), lincheck::Value(1)}
    };

    lincheck::LinearizabilityVerifier verifier(spec);
    require(!verifier.verify(scenario, impossible), "verifier should reject impossible repeated counter results");
    require(state_key_calls > 0, "verifier should use the optional sequential state key");
}

void verifier_checks_hand_authored_stack_histories() {
    lincheck::TestSpec spec = lincheck::test<SequentialStack, SequentialStack>()
        .operation("push", &SequentialStack::push, &SequentialStack::push, lincheck::range<int>(0, 9))
        .operation("pop", &SequentialStack::pop, &SequentialStack::pop);

    lincheck::ExecutionScenario scenario;
    scenario.init = {
        lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {lincheck::Value(1)}},
        lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {lincheck::Value(2)}}
    };
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 1, .name = "pop", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "pop", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    lincheck::ExecutionResult valid;
    valid.init_results = {lincheck::Value(), lincheck::Value()};
    valid.parallel_results = {{lincheck::Value(1)}, {lincheck::Value(2)}};

    lincheck::LinearizabilityVerifier verifier(spec);
    require(verifier.verify(scenario, valid), "overlapping stack pops may linearize in LIFO order");

    lincheck::ExecutionResult invalid = valid;
    invalid.parallel_results = {{lincheck::Value(2)}, {lincheck::Value(2)}};
    require(!verifier.verify(scenario, invalid), "stack verifier should reject duplicate pop result");
}

void verifier_checks_hand_authored_queue_histories() {
    lincheck::TestSpec spec = lincheck::test<SequentialQueue, SequentialQueue>()
        .operation("push", &SequentialQueue::push, &SequentialQueue::push, lincheck::range<int>(0, 9))
        .operation("pop", &SequentialQueue::pop, &SequentialQueue::pop);

    lincheck::ExecutionScenario scenario;
    scenario.init = {
        lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {lincheck::Value(1)}},
        lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {lincheck::Value(2)}}
    };
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 1, .name = "pop", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 1, .name = "pop", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    lincheck::ExecutionResult valid;
    valid.init_results = {lincheck::Value(), lincheck::Value()};
    valid.parallel_results = {{lincheck::Value(2)}, {lincheck::Value(1)}};

    lincheck::LinearizabilityVerifier verifier(spec);
    require(verifier.verify(scenario, valid), "overlapping queue pops may linearize in FIFO order");

    lincheck::ExecutionResult invalid = valid;
    invalid.parallel_results = {{lincheck::Value(1)}, {lincheck::Value(1)}};
    require(!verifier.verify(scenario, invalid), "queue verifier should reject duplicate pop result");
}

void model_checker_finds_queue_like_capacity_bug() {
    lincheck::TestSpec spec = lincheck::test<BrokenSingleSlotQueue, SequentialSingleSlotQueue>()
        .operation(
            "push",
            &BrokenSingleSlotQueue::push,
            &SequentialSingleSlotQueue::push,
            lincheck::range<int>(1, 9)
        );

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .seed(7)
        .max_schedule_length(3)
        .check(spec);

    require(!result.success, "model checker should find single-slot queue capacity bug");
    require(result.failure == lincheck::FailureKind::invalid_results, "queue bug should have invalid-results failure kind");
    require(result.trace.find("push(") != std::string::npos, "queue failure should include scenario");
}

void model_checker_accepts_optional_queue_results() {
    lincheck::TestSpec spec = lincheck::test<OptionalLockedQueue, OptionalSequentialQueue>()
        .concurrent_factory([] {
            return std::make_shared<OptionalLockedQueue>(1);
        })
        .sequential_factory([] {
            return std::make_shared<OptionalSequentialQueue>(1);
        })
        .operation("pop", &OptionalLockedQueue::pop, &OptionalSequentialQueue::pop);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(4)
        .check(spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "model checker should accept queue operations returning std::optional");
}

void model_checker_accepts_expected_operation_exceptions() {
    lincheck::TestSpec spec = lincheck::test<ThrowingPopQueue, ThrowingPopSequentialQueue>()
        .concurrent_factory([] {
            return std::make_shared<ThrowingPopQueue>(1);
        })
        .sequential_factory([] {
            return std::make_shared<ThrowingPopSequentialQueue>(1);
        })
        .operation_with_options(
            "pop",
            &ThrowingPopQueue::pop,
            &ThrowingPopSequentialQueue::pop,
            lincheck::exceptions_as_results()
        );

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(4)
        .check(spec);

    if (!result.success) {
        std::cerr << result.message << "\n" << result.trace << "\n";
    }
    require(result.success, "model checker should compare opted-in operation exceptions as results");
}

void model_checker_rejects_mismatched_operation_exception_results() {
    lincheck::TestSpec spec = lincheck::test<ThrowingPopQueue, ReturningEmptySequentialQueue>()
        .operation_with_options(
            "pop",
            &ThrowingPopQueue::pop,
            &ReturningEmptySequentialQueue::pop,
            lincheck::exceptions_as_results()
        );

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "model checker should reject exception results that do not match the sequential spec");
    require(result.failure == lincheck::FailureKind::invalid_results, "mismatched operation exception should be an invalid-results failure");
    require(result.trace.find("operation.throw") != std::string::npos, "expected exception result failure should retain operation throw trace");
    require(result.trace.find("exception(") != std::string::npos, "expected exception result failure should show the exception value");
    require(result.trace.find("empty") != std::string::npos, "expected exception result failure should include the exception message");
    require(result.verifier_explanation.find("sequential model returned -1") != std::string::npos, "verifier explanation should compare exception result to sequential value");
}

void generator_respects_non_parallel_operation_groups() {
    lincheck::TestSpec spec = lincheck::test<GroupedOperations, GroupedOperations>()
        .operation_with_options(
            "first",
            &GroupedOperations::first,
            &GroupedOperations::first,
            lincheck::non_parallel_group("exclusive")
        )
        .operation_with_options(
            "second",
            &GroupedOperations::second,
            &GroupedOperations::second,
            lincheck::non_parallel_group("exclusive")
        );

    lincheck::RandomExecutionGenerator generator(
        spec,
        4,
        4,
        0,
        0,
        0
    );
    const auto scenario = generator.next();

    int exclusive_count = 0;
    std::optional<std::size_t> exclusive_thread;
    for (std::size_t thread_index = 0; thread_index < scenario.parallel.size(); ++thread_index) {
        const auto& thread = scenario.parallel[thread_index];
        for (const auto& actor : thread) {
            if (actor.group == "exclusive") {
                ++exclusive_count;
                if (!exclusive_thread) {
                    exclusive_thread = thread_index;
                }
                require(*exclusive_thread == thread_index, "non-parallel group should appear in only one parallel thread");
                require(actor.non_parallel, "actor should preserve non-parallel metadata");
                require(
                    actor.to_string().find("[group=exclusive non_parallel]") != std::string::npos,
                    "actor trace text should include non-parallel group metadata"
                );
            }
        }
    }

    require(exclusive_count == 4, "non-parallel group should be allowed to repeat sequentially in its owning thread");
}

void generator_preserves_plain_operation_group_metadata() {
    lincheck::TestSpec spec = lincheck::test<GroupedOperations, GroupedOperations>()
        .operation_with_options(
            "ordinary",
            &GroupedOperations::ordinary,
            &GroupedOperations::ordinary,
            lincheck::operation_group("plain")
        );

    lincheck::RandomExecutionGenerator generator(
        spec,
        1,
        1,
        0,
        0,
        0
    );
    const auto scenario = generator.next();

    require(scenario.parallel.size() == 1, "plain group scenario should have one thread");
    require(scenario.parallel[0].size() == 1, "plain group scenario should have one actor");
    const auto& actor = scenario.parallel[0][0];
    require(actor.group == "plain", "actor should preserve plain operation group metadata");
    require(!actor.non_parallel, "plain operation group should not imply non-parallel metadata");
    require(actor.to_string() == "ordinary() [group=plain]", "actor trace text should include plain group metadata");
}

void generator_respects_one_shot_operations() {
    lincheck::TestSpec spec = lincheck::test<GroupedOperations, GroupedOperations>()
        .operation_with_options(
            "first",
            &GroupedOperations::first,
            &GroupedOperations::first,
            lincheck::one_shot()
        );

    lincheck::RandomExecutionGenerator generator(
        spec,
        3,
        3,
        3,
        3,
        0
    );
    const auto scenario = generator.next();

    int one_shot_count = 0;
    auto count_actor = [&](const lincheck::Actor& actor) {
        if (actor.one_shot) {
            ++one_shot_count;
            require(
                actor.to_string().find("[one_shot]") != std::string::npos,
                "actor trace text should include one-shot metadata"
            );
        }
    };

    for (const auto& actor : scenario.init) count_actor(actor);
    for (const auto& thread : scenario.parallel) {
        for (const auto& actor : thread) count_actor(actor);
    }
    for (const auto& actor : scenario.post) count_actor(actor);

    require(one_shot_count == 1, "one-shot operation should be generated at most once per scenario");
}

void generator_resets_stateful_parameter_generators_per_scenario() {
    auto stateful = lincheck::custom([next = 0](std::mt19937_64&) mutable {
        return ++next;
    });

    lincheck::TestSpec spec = lincheck::test<SequentialStack, SequentialStack>()
        .operation("push", &SequentialStack::push, &SequentialStack::push, stateful);

    lincheck::RandomExecutionGenerator generator(
        spec,
        1,
        2,
        0,
        0,
        0
    );

    const auto first = generator.next();
    const auto second = generator.next();

    require(first.parallel.size() == 1 && first.parallel[0].size() == 2, "first scenario should contain two generated actors");
    require(second.parallel.size() == 1 && second.parallel[0].size() == 2, "second scenario should contain two generated actors");
    require(lincheck::value_cast<int>(first.parallel[0][0].arguments[0]) == 1, "first scenario should start at first generated value");
    require(lincheck::value_cast<int>(first.parallel[0][1].arguments[0]) == 2, "first scenario should advance generator state within scenario");
    require(lincheck::value_cast<int>(second.parallel[0][0].arguments[0]) == 1, "second scenario should reset generator state");
    require(lincheck::value_cast<int>(second.parallel[0][1].arguments[0]) == 2, "second scenario should advance after reset");
}

void value_supports_stable_hashing() {
    std::unordered_set<lincheck::Value, lincheck::ValueHash> values;
    values.insert(lincheck::Value(1));
    values.insert(lincheck::Value(1));
    values.insert(lincheck::Value(2));
    values.insert(lincheck::Value("one"));

    require(values.size() == 3, "ValueHash should allow stable hash-based Value sets");
    require(lincheck::Value("one").stable_hash() == lincheck::Value(std::string("one")).stable_hash(), "equal strings should have identical stable hashes");
    require(lincheck::Value(GeneratedChoice::second) == lincheck::Value(2u), "enum values should convert through their underlying type");
}

void value_supports_custom_user_types() {
    const CustomResult raw{7, "seven"};
    const lincheck::Value first(raw);
    const lincheck::Value second(CustomResult{7, "seven"});
    const lincheck::Value different(CustomResult{8, "eight"});

    require(first == second, "custom values with equal payloads should compare equal");
    require(!(first == different), "custom values with different payloads should not compare equal");
    require(first.stable_hash() == second.stable_hash(), "equal custom values should have equal stable hashes");
    require(first.to_string() == "custom(7,seven)", "custom values should use ostream formatting by default");
    require(lincheck::value_cast<CustomResult>(first) == raw, "value_cast should recover custom payloads");

    struct ExplicitOnly {
        int value = 0;
    };

    const auto explicit_first = lincheck::Value::custom(
        ExplicitOnly{4},
        [](const ExplicitOnly& value) { return "explicit(" + std::to_string(value.value) + ")"; },
        [](const ExplicitOnly& value) { return static_cast<std::uint64_t>(value.value * 17); },
        [](const ExplicitOnly& left, const ExplicitOnly& right) { return left.value == right.value; }
    );
    const auto explicit_second = lincheck::Value::custom(
        ExplicitOnly{4},
        [](const ExplicitOnly& value) { return "explicit(" + std::to_string(value.value) + ")"; },
        [](const ExplicitOnly& value) { return static_cast<std::uint64_t>(value.value * 17); },
        [](const ExplicitOnly& left, const ExplicitOnly& right) { return left.value == right.value; }
    );

    require(explicit_first == explicit_second, "explicit custom adapters should define equality");
    require(explicit_first.to_string() == "explicit(4)", "explicit custom adapters should define formatting");
    require(explicit_first.stable_hash() == explicit_second.stable_hash(), "explicit custom adapters should define stable hashing");
    require(lincheck::value_cast<ExplicitOnly>(explicit_first).value == 4, "value_cast should recover explicitly adapted payloads");
}

void value_supports_std_optional_payloads() {
    const lincheck::Value empty(std::optional<int>{});
    const lincheck::Value empty_again(std::optional<int>{});
    const lincheck::Value one(std::optional<int>{1});
    const lincheck::Value two(std::optional<int>{2});

    require(empty == empty_again, "equal empty optionals should compare equal");
    require(!(empty == lincheck::Value()), "std::optional nullopt should stay distinct from void results");
    require(!(one == two), "different optional payloads should not compare equal");
    require(empty.to_string() == "optional(nullopt)", "empty optional should have a readable representation");
    require(one.to_string() == "optional(1)", "present optional should format the payload");
    require(empty.stable_hash() == empty_again.stable_hash(), "equal optional values should have equal stable hashes");
    require(lincheck::value_cast<std::optional<int>>(empty) == std::nullopt, "value_cast should recover empty optionals");
    require(lincheck::value_cast<std::optional<int>>(one).value() == 1, "value_cast should recover present optionals");

    auto domain = lincheck::values<std::optional<int>>({std::optional<int>{}, std::optional<int>{3}});
    std::mt19937_64 rng(11);
    bool saw_empty = false;
    bool saw_present = false;
    for (int i = 0; i < 20; ++i) {
        const auto value = lincheck::value_cast<std::optional<int>>(domain(rng));
        saw_empty = saw_empty || !value.has_value();
        saw_present = saw_present || value == std::optional<int>{3};
    }
    require(saw_empty && saw_present, "optional domains should round-trip through generated Values");
}

void parameter_generators_support_plan_builtins_and_custom_seeded_values() {
    std::mt19937_64 rng(123);

    const auto bool_value = lincheck::booleans()(rng);
    require(std::holds_alternative<bool>(bool_value.storage()), "booleans generator should produce bool values");

    const auto default_bool = lincheck::gen<bool>()(rng);
    require(std::holds_alternative<bool>(default_bool.storage()), "generic bool generator should produce bool values");

    auto default_int = lincheck::gen<int>();
    for (int i = 0; i < 20; ++i) {
        const int value = lincheck::value_cast<int>(default_int(rng));
        require(value >= -2 && value <= 2, "generic signed integer generator should use a small default domain");
    }

    auto default_unsigned = lincheck::gen<unsigned>();
    for (int i = 0; i < 20; ++i) {
        const auto value = lincheck::value_cast<unsigned>(default_unsigned(rng));
        require(value <= 4u, "generic unsigned integer generator should use a small default domain");
    }

    auto default_enum = lincheck::gen<GeneratedChoice>();
    for (int i = 0; i < 20; ++i) {
        const auto value = lincheck::value_cast<GeneratedChoice>(default_enum(rng));
        require(static_cast<unsigned>(value) <= 4u, "generic enum generator should use the unsigned underlying default domain");
    }

    auto default_double = lincheck::gen<double>();
    for (int i = 0; i < 20; ++i) {
        const auto value = lincheck::value_cast<double>(default_double(rng));
        require(value >= -1.0 && value <= 1.0, "generic floating-point generator should use a small default domain");
    }

    auto default_text = lincheck::gen<std::string>();
    for (int i = 0; i < 20; ++i) {
        const auto value = lincheck::value_cast<std::string>(default_text(rng));
        require(value.size() <= 4, "generic string generator should use a short default domain");
    }

    auto fixed_int = lincheck::values<int>({3, 5});
    for (int i = 0; i < 20; ++i) {
        const int value = lincheck::value_cast<int>(fixed_int(rng));
        require(value == 3 || value == 5, "fixed value generator should produce only domain values");
    }

    auto random_string = lincheck::strings(2, 4, "ab");
    for (int i = 0; i < 20; ++i) {
        const auto value = lincheck::value_cast<std::string>(random_string(rng));
        require(value.size() >= 2 && value.size() <= 4, "string generator should respect length bounds");
        require(value.find_first_not_of("ab") == std::string::npos, "string generator should use the requested alphabet");
    }

    auto fixed_string = lincheck::strings({"left", "right"});
    const auto chosen_string = lincheck::value_cast<std::string>(fixed_string(rng));
    require(chosen_string == "left" || chosen_string == "right", "string domain generator should produce fixed values");

    auto enum_domain = lincheck::values<GeneratedChoice>({GeneratedChoice::first, GeneratedChoice::second});
    const auto choice = lincheck::value_cast<GeneratedChoice>(enum_domain(rng));
    require(choice == GeneratedChoice::first || choice == GeneratedChoice::second, "enum domain generator should round-trip enum values");

    auto custom = lincheck::custom([](std::mt19937_64& seeded) {
        return static_cast<int>(seeded() % 100);
    });
    std::mt19937_64 first_rng(7);
    std::mt19937_64 second_rng(7);
    require(
        lincheck::value_cast<int>(custom(first_rng)) == lincheck::value_cast<int>(custom(second_rng)),
        "custom generator should be deterministic for the same seed"
    );

    lincheck::TestSpec spec = lincheck::test<GeneratedParameters, GeneratedParameters>()
        .operation(
            "record",
            &GeneratedParameters::record,
            &GeneratedParameters::record,
            lincheck::gen<bool>(),
            lincheck::gen<std::string>(),
            lincheck::gen<GeneratedChoice>(),
            lincheck::gen<int>()
        );

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check(spec);

    require(result.success, "registered operations should accept generated bool/string/enum/int arguments");
}

void stress_runner_executes_correct_counter() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(5)
        .invocations_per_iteration(10)
        .threads(2)
        .actors_per_thread(2)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(result.success, "stress runner should accept atomic fetch_add counter");
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.kind == "operation.start" &&
                record.operation &&
                record.operation->name == "inc";
        }),
        "stress success result should retain structured operation trace events"
    );
}

void stress_runner_finds_forced_broken_counter_overlap() {
    lincheck::TestSpec spec = lincheck::test<StressBrokenCounter, SequentialCounter>()
        .operation("inc", &StressBrokenCounter::inc, &SequentialCounter::inc);

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}},
        {lincheck::Actor{.operation_index = 0, .name = "inc", .group = "", .non_parallel = false, .one_shot = false, .arguments = {}}}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(100))
        .check(spec, scenario);

    require(!result.success, "stress runner should find the forced broken counter overlap");
    require(result.failure == lincheck::FailureKind::invalid_results, "broken stress counter should fail linearizability");
    require(!result.verifier_explanation.empty(), "broken stress counter should include verifier explanation");
    require(result.trace.find("stress invocation found non-linearizable result") != std::string::npos, "stress counter failure should name the stress invalid-result path");
    require(result.trace.find("thread interleaving:\n") != std::string::npos, "stress counter failure should include thread interleaving");
    require(result.trace.find("operation clocks:") != std::string::npos, "stress counter failure should include operation clocks");
}

void stress_runner_finds_forced_broken_queue_overlap() {
    lincheck::TestSpec spec = lincheck::test<StressBrokenSingleSlotQueue, SequentialSingleSlotQueue>()
        .operation(
            "push",
            &StressBrokenSingleSlotQueue::push,
            &SequentialSingleSlotQueue::push,
            lincheck::values<int>({1, 2})
        );

    lincheck::ExecutionScenario scenario;
    scenario.parallel = {
        {lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {lincheck::Value(1)}}},
        {lincheck::Actor{.operation_index = 0, .name = "push", .group = "", .non_parallel = false, .one_shot = false, .arguments = {lincheck::Value(2)}}}
    };

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .invocation_timeout(std::chrono::milliseconds(100))
        .check(spec, scenario);

    require(!result.success, "stress runner should find the forced broken queue overlap");
    require(result.failure == lincheck::FailureKind::invalid_results, "broken stress queue should fail linearizability");
    require(!result.verifier_explanation.empty(), "broken stress queue should include verifier explanation");
    require(result.trace.find("push(1)") != std::string::npos, "stress queue failure should include the first push actor");
    require(result.trace.find("push(2)") != std::string::npos, "stress queue failure should include the second push actor");
}

void stress_runner_releases_parallel_workers_after_ready_gate() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc)
        .validation([](const AtomicCounter&) {
            return std::string{"forced validation failure"};
        });

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(3)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(!result.success, "forced validation should make the stress trace visible");
    require(result.failure == lincheck::FailureKind::validation_failure, "forced validation should preserve failure kind");

    const auto start_position = result.trace.find("stress.parallel.start threads=3");
    require(start_position != std::string::npos, "stress trace should include the parallel start release point");
    for (int thread = 0; thread < 3; ++thread) {
        const auto ready = "stress.worker.ready thread=" + std::to_string(thread);
        const auto ready_position = result.trace.find(ready);
        require(ready_position != std::string::npos, "stress trace should include every worker ready point");
        require(ready_position < start_position, "stress runner should release parallel work only after every worker is ready");
    }
}

void stress_runner_reports_non_seq_cst_memory_order_warning_on_success() {
    lincheck::TestSpec spec = lincheck::test<RelaxedAtomicCounter, SequentialCounter>()
        .operation("inc", &RelaxedAtomicCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(result.success, "stress runner should accept relaxed fetch_add counter");
    require(
        has_warning(result, "atomic.fetch_add", "sequentially consistent"),
        "stress runner should return non-seq_cst atomic warning on success without timeout tracing"
    );
    require(
        has_memory_event(result, lincheck::MemoryEventKind::atomic_fetch_add),
        "successful stress checks should expose representative structured memory events"
    );
    require(
        has_memory_event_object_id(result, lincheck::MemoryEventKind::atomic_fetch_add),
        "successful stress checks should retain stable object IDs in memory events"
    );
}

void stress_runner_executes_locked_counter() {
    lincheck::TestSpec spec = lincheck::test<LockedCounter, SequentialCounter>()
        .operation("inc", &LockedCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(3)
        .invocations_per_iteration(5)
        .threads(2)
        .actors_per_thread(2)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(result.success, "stress runner should accept mutex-protected counter");
}

void lock_guard_supports_raii_and_adopt_lock() {
    lincheck::mutex mutex;
    RecordingRuntime runtime;

    {
        lincheck::ScopedRuntime scoped(&runtime);
        {
            lincheck::lock_guard guard(mutex);
        }
    }
    require(mutex.try_lock(), "lock_guard destructor should release an owned mutex");
    mutex.unlock();

    mutex.lock();
    {
        lincheck::lock_guard adopted(mutex, std::adopt_lock);
    }
    require(mutex.try_lock(), "lock_guard adopt_lock constructor should release the adopted mutex on destruction");
    mutex.unlock();

    auto has_event = [&](const std::string& needle) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };
    require(has_event("mutex.lock object=obj#"), "lock_guard should trace mutex lock through lincheck::mutex");
    require(has_event("mutex.unlock object=obj#"), "lock_guard should trace mutex unlock through lincheck::mutex");
}

void recursive_mutex_supports_reentrant_locking_and_raii() {
    lincheck::recursive_mutex mutex;
    RecordingRuntime runtime;

    {
        lincheck::ScopedRuntime scoped(&runtime);
        mutex.lock();
        mutex.lock();
        require(mutex.try_lock(), "recursive_mutex try_lock should reacquire on the owning thread");
        mutex.unlock();
        mutex.unlock();
        mutex.unlock();
        {
            lincheck::lock_guard guard(mutex);
            lincheck::lock_guard nested_guard(mutex);
        }
        {
            lincheck::unique_lock lock(mutex);
            require(lock.owns_lock(), "unique_lock should work with lincheck::recursive_mutex");
        }
        {
            lincheck::scoped_lock scoped_lock(mutex);
        }
    }

    require(mutex.try_lock(), "recursive_mutex should be unlocked after RAII guards leave scope");
    mutex.unlock();

    auto has_event = [&](const std::string& needle) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };
    require(has_event("recursive_mutex.lock object=obj#"), "recursive_mutex lock should use stable object IDs");
    require(has_event("recursive_mutex.try_lock object=obj#"), "recursive_mutex try_lock should use stable object IDs");
    require(has_event("recursive_mutex.unlock object=obj#"), "recursive_mutex unlock should use stable object IDs");
}

void timed_mutex_supports_timed_locking_and_raii() {
    lincheck::timed_mutex mutex;
    require(mutex.try_lock_for(std::chrono::milliseconds(0)), "timed_mutex try_lock_for should acquire an unlocked mutex");
    mutex.unlock();
    require(
        mutex.try_lock_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)),
        "timed_mutex try_lock_until should acquire an unlocked mutex"
    );
    mutex.unlock();

    lincheck::unique_lock deferred(mutex, std::defer_lock);
    require(deferred.try_lock_for(std::chrono::milliseconds(0)), "unique_lock try_lock_for should work with timed_mutex");
    deferred.unlock();
    require(
        deferred.try_lock_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)),
        "unique_lock try_lock_until should work with timed_mutex"
    );
    deferred.unlock();

    lincheck::recursive_timed_mutex recursive;
    recursive.lock();
    require(recursive.try_lock_for(std::chrono::milliseconds(0)), "recursive_timed_mutex should reacquire on owning thread");
    recursive.unlock();
    require(
        recursive.try_lock_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)),
        "recursive_timed_mutex try_lock_until should reacquire on owning thread"
    );
    recursive.unlock();
    recursive.unlock();
}

void shared_mutex_supports_shared_and_exclusive_locking() {
    lincheck::shared_mutex mutex;
    lincheck::shared_lock first(mutex);
    lincheck::shared_lock second(mutex, std::try_to_lock);
    require(second.owns_lock(), "shared_mutex should allow multiple shared owners");
    bool exclusive_acquired = true;
    std::thread exclusive_contender([&] {
        exclusive_acquired = mutex.try_lock();
        if (exclusive_acquired) {
            mutex.unlock();
        }
    });
    exclusive_contender.join();
    require(!exclusive_acquired, "exclusive try_lock should fail while shared locks are held");
    second.unlock();
    first.unlock();

    require(mutex.try_lock(), "exclusive try_lock should acquire after shared locks are released");
    bool shared_acquired = true;
    std::thread shared_contender([&] {
        shared_acquired = mutex.try_lock_shared();
        if (shared_acquired) {
            mutex.unlock_shared();
        }
    });
    shared_contender.join();
    require(!shared_acquired, "shared try_lock should fail while an exclusive lock is held");
    mutex.unlock();

    mutex.lock_shared();
    {
        lincheck::shared_lock adopted(mutex, std::adopt_lock);
        require(adopted.owns_lock(), "shared_lock adopt_lock should mark the lock as owned");
    }
    require(mutex.try_lock(), "shared_lock destructor should release an adopted shared lock");
    mutex.unlock();

    lincheck::shared_lock released_lock(mutex);
    lincheck::shared_mutex* released = released_lock.release();
    require(released == &mutex, "shared_lock release should return the associated mutex");
    require(!released_lock.owns_lock(), "shared_lock release should clear ownership");
    require(released_lock.mutex() == nullptr, "shared_lock release should clear the associated mutex");
    released->unlock_shared();

    lincheck::shared_timed_mutex timed;
    require(
        timed.try_lock_shared_for(std::chrono::milliseconds(0)),
        "shared_timed_mutex try_lock_shared_for should acquire an unlocked mutex"
    );
    timed.unlock_shared();

    lincheck::shared_lock timed_guard(timed, std::defer_lock);
    require(
        timed_guard.try_lock_for(std::chrono::milliseconds(0)),
        "shared_lock try_lock_for should work with shared_timed_mutex"
    );
    timed_guard.unlock();
    require(
        timed_guard.try_lock_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)),
        "shared_lock try_lock_until should work with shared_timed_mutex"
    );
    timed_guard.unlock();

    require(timed.try_lock_for(std::chrono::milliseconds(0)), "shared_timed_mutex exclusive timed lock should work");
    bool timed_shared_acquired = true;
    std::thread timed_shared_contender([&] {
        timed_shared_acquired = timed.try_lock_shared();
        if (timed_shared_acquired) {
            timed.unlock_shared();
        }
    });
    timed_shared_contender.join();
    require(!timed_shared_acquired, "shared_timed_mutex shared try_lock should fail while exclusive lock is held");
    timed.unlock();
}

void scoped_lock_supports_variadic_raii_and_adopt_lock() {
    lincheck::mutex first;
    lincheck::mutex second;
    RecordingRuntime runtime;

    {
        lincheck::ScopedRuntime scoped(&runtime);
        {
            lincheck::scoped_lock empty;
        }
        {
            lincheck::scoped_lock single(first);
        }
        {
            lincheck::scoped_lock pair(first, second);
        }
    }
    require(first.try_lock(), "scoped_lock destructor should release the first owned mutex");
    first.unlock();
    require(second.try_lock(), "scoped_lock destructor should release the second owned mutex");
    second.unlock();

    first.lock();
    second.lock();
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::scoped_lock adopted(std::adopt_lock, first, second);
    }
    require(first.try_lock(), "scoped_lock adopt_lock constructor should release the first adopted mutex");
    first.unlock();
    require(second.try_lock(), "scoped_lock adopt_lock constructor should release the second adopted mutex");
    second.unlock();

    const auto count_events = [&](const std::string& needle) {
        return std::count_if(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };
    require(count_events("mutex.lock object=obj#") >= 1, "scoped_lock should trace mutex locks through lincheck::mutex");
    require(count_events("mutex.unlock object=obj#") >= 5, "scoped_lock should unlock every owned or adopted mutex");
}

void stress_runner_reports_verifier_explanation_for_invalid_results() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, WrongSequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &WrongSequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(!result.success, "stress runner should reject results that do not match the sequential spec");
    require(result.failure == lincheck::FailureKind::invalid_results, "wrong sequential spec should produce invalid-results failure");
    require(!result.verifier_explanation.empty(), "stress invalid-results failure should expose verifier explanation field");
    require(result.trace.find("failure:\n  kind: invalid_results") != std::string::npos, "stress invalid-results trace should include failure summary");
    require(result.trace.find("verifier explanation:") != std::string::npos, "stress invalid-results trace should include verifier explanation");
    require(result.trace.find("interleaving:\n") != std::string::npos, "stress invalid-results trace should include interleaving section");
    require(result.trace.find("thread interleaving:\n") != std::string::npos, "stress invalid-results trace should include thread-by-thread interleaving section");
    require(result.trace.find("operation clocks:") != std::string::npos, "stress invalid-results trace should include operation clocks");
    require(result.trace.find("parallel:\n") != std::string::npos, "stress invalid-results trace should include scenario section");
    require(result.trace.find("stress runtime trace:\n") != std::string::npos, "stress invalid-results trace should include runtime trace section");
    require(result.trace.find("operation.start") != std::string::npos, "stress invalid-results trace should include operation start events");
    require(result.trace.find("operation.finish") != std::string::npos, "stress invalid-results trace should include operation finish events");
    require(
        result.verifier_explanation.find("sequential model returned") != std::string::npos,
        "stress verifier explanation should include the mismatched sequential return"
    );
}

void unique_lock_supports_standard_lock_tags() {
    lincheck::mutex mutex;

    lincheck::unique_lock deferred(mutex, std::defer_lock);
    require(!deferred.owns_lock(), "defer_lock constructor should not lock the mutex");
    deferred.lock();
    require(deferred.owns_lock(), "deferred unique_lock should own the mutex after lock");
    deferred.unlock();

    lincheck::unique_lock try_lock(mutex, std::try_to_lock);
    require(try_lock.owns_lock(), "try_to_lock constructor should acquire an unlocked mutex");
    try_lock.unlock();

    mutex.lock();
    {
        lincheck::unique_lock adopted(mutex, std::adopt_lock);
        require(adopted.owns_lock(), "adopt_lock constructor should mark the lock as owned");
    }

    lincheck::unique_lock after_adopt(mutex, std::try_to_lock);
    require(after_adopt.owns_lock(), "adopted lock should unlock in the unique_lock destructor");
    require(after_adopt.mutex() == &mutex, "unique_lock mutex() should return the associated mutex");
    require(after_adopt.mutex_ptr() == &mutex, "unique_lock mutex_ptr() should remain an alias for the associated mutex");
    lincheck::mutex* released = after_adopt.release();
    require(released == &mutex, "unique_lock release should return the associated mutex");
    require(after_adopt.mutex() == nullptr, "unique_lock release should clear the associated mutex");
    require(!after_adopt.owns_lock(), "unique_lock release should clear ownership");
    released->unlock();

    lincheck::mutex other_mutex;
    lincheck::unique_lock unlocked(mutex, std::defer_lock);
    lincheck::unique_lock locked(other_mutex);
    unlocked.swap(locked);
    require(unlocked.owns_lock(), "unique_lock member swap should move ownership to the target lock");
    require(unlocked.mutex() == &other_mutex, "unique_lock member swap should move the associated mutex");
    require(!locked.owns_lock(), "unique_lock member swap should move non-ownership to the other lock");
    require(locked.mutex() == &mutex, "unique_lock member swap should preserve the other associated mutex");
    swap(unlocked, locked);
    require(locked.owns_lock(), "unique_lock ADL swap should move ownership to the target lock");
    require(locked.mutex() == &other_mutex, "unique_lock ADL swap should move the associated mutex");
    require(!unlocked.owns_lock(), "unique_lock ADL swap should move non-ownership to the other lock");
    require(unlocked.mutex() == &mutex, "unique_lock ADL swap should preserve the other associated mutex");
}

void stress_runner_reports_cooperative_timeout() {
    lincheck::TestSpec spec = lincheck::test<TimeoutSpin, SequentialCounter>()
        .operation("spin", &TimeoutSpin::spin, &SequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .invocation_timeout(std::chrono::milliseconds(1))
        .check(spec);

    require(!result.success, "stress runner should report a cooperative timeout");
    require(result.failure == lincheck::FailureKind::timeout, "cooperative timeout should have timeout failure kind");
    require(result.message == "timeout", "cooperative timeout should preserve timeout message");
    require(result.trace.find("failure:\n  kind: timeout") != std::string::npos, "cooperative timeout trace should include failure summary");
    require(result.trace.find("timeout.spin") != std::string::npos, "cooperative timeout trace should include switch-point location");
    require(result.trace.find("thread interleaving:\n") != std::string::npos, "cooperative timeout trace should include thread interleaving context");
    require(result.trace.find("operation clocks:") != std::string::npos, "cooperative timeout trace should include operation clock context");
    require(result.trace.find("parallel:\n") != std::string::npos, "cooperative timeout trace should include scenario context");
}

void stress_runner_trace_filter_does_not_disable_timeout() {
    lincheck::TestSpec spec = lincheck::test<TimeoutSpin, SequentialCounter>()
        .operation("spin", &TimeoutSpin::spin, &SequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .invocation_timeout(std::chrono::milliseconds(1))
        .trace_exclude("timeout.spin")
        .check(spec);

    require(!result.success, "stress runner should still report a timeout when trace lines are filtered");
    require(result.failure == lincheck::FailureKind::timeout, "filtered stress timeout should keep timeout failure kind");
    require(result.trace.find("timeout.spin") == std::string::npos, "filtered stress timeout should suppress matching runtime line");
    require(result.trace.find("parallel:\n") != std::string::npos, "filtered stress timeout should keep scenario context");
}

void stress_runner_trace_filter_applies_without_timeout() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc)
        .validation([](const AtomicCounter&) {
            return std::string{"forced validation failure"};
        });

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .trace_include("operation.start")
        .check(spec);

    require(!result.success, "filtered stress validation failure should still run verification");
    require(result.failure == lincheck::FailureKind::validation_failure, "filtered stress failure should keep validation failure kind");
    require(result.trace.find("stress runtime trace:\n") != std::string::npos, "filtered stress failure should include runtime trace section");
    require(result.trace.find("operation.start") != std::string::npos, "included stress runtime line should be retained");
    require(result.trace.find("operation.finish") == std::string::npos, "non-included stress runtime line should be filtered");
    require(result.trace.find("parallel:\n") != std::string::npos, "stress trace filtering should not remove scenario context");
}

void stress_runner_reports_validation_failure() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc)
        .state_representation([](const AtomicCounter& counter) {
            return "value=" + std::to_string(counter.value.load());
        })
        .validation([](const AtomicCounter& counter) {
            return counter.value.load() == 0 ? std::string{} : std::string{"counter should be zero"};
        });

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(!result.success, "stress runner should report validation failure");
    require(result.failure == lincheck::FailureKind::validation_failure, "stress validation failure should have validation failure kind");
    require(result.message == "counter should be zero", "stress validation failure should preserve validation message");
    require(result.trace.find("validation failed: counter should be zero") != std::string::npos, "stress validation trace should include validation message");
    require(result.trace.find("interleaving:\n") != std::string::npos, "stress validation trace should include interleaving section");
    require(result.trace.find("thread interleaving:\n") != std::string::npos, "stress validation trace should include thread interleaving section");
    require(result.trace.find("operation clocks:") != std::string::npos, "stress validation trace should include operation clocks");
    require(result.trace.find("state:\n  value=") != std::string::npos, "stress validation trace should include state representation");
    require(result.trace.find("parallel:\n") != std::string::npos, "stress validation trace should include scenario section");
}

void stress_runner_preserves_unexpected_exception_ptr() {
    lincheck::TestSpec spec = lincheck::test<ThrowingOperation, SequentialCounter>()
        .operation("fail", &ThrowingOperation::fail, &SequentialCounter::inc);

    const auto result = lincheck::StressOptions()
        .iterations(1)
        .invocations_per_iteration(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .check(spec);

    require(!result.success, "stress runner should report throwing operation");
    require(result.failure == lincheck::FailureKind::unexpected_exception, "stress exception should have unexpected-exception kind");
    require(result.message == "operation boom", "stress exception should preserve formatted message");
    require_exception_message(result.exception, "operation boom", "stress exception should preserve exception_ptr");
    require(result.trace.find("failure:\n  kind: unexpected_exception") != std::string::npos, "stress exception trace should include failure summary");
    require(result.trace.find("operation.throw") != std::string::npos, "stress exception trace should include operation throw");
    require(result.trace.find("operation.throw thread=0 actor=0 fail() -> exception(") != std::string::npos, "stress operation throw trace should include the exception value");
    require(result.trace.find("thread interleaving:\n") != std::string::npos, "stress exception trace should include thread interleaving context");
    require(result.trace.find("operation clocks:") != std::string::npos, "stress exception trace should include operation clock context");
    require(result.trace.find("thread 0 actor 0 fail() -> exception(") != std::string::npos, "stress operation clocks should include the throwing operation result");
    require(result.trace.find("parallel:\n") != std::string::npos, "stress exception trace should include scenario context");
}

void model_checker_reports_validation_failure() {
    lincheck::TestSpec spec = lincheck::test<AtomicCounter, SequentialCounter>()
        .operation("inc", &AtomicCounter::inc, &SequentialCounter::inc)
        .validation([](const AtomicCounter& counter) {
            return counter.value.load() == 0;
        });

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check(spec);

    require(!result.success, "model checker should report validation failure");
    require(result.failure == lincheck::FailureKind::validation_failure, "model checker validation failure should have validation failure kind");
    require(result.message == "validation failed", "bool validation failure should use the default message");
    require(result.trace.find("schedule:") != std::string::npos, "model checker validation trace should include schedule");
    require(result.trace.find("validation failed: validation failed") != std::string::npos, "model checker validation trace should include validation message");
}

void model_checker_preserves_unexpected_exception_ptr() {
    lincheck::TestSpec spec = lincheck::test<ThrowingOperation, SequentialCounter>()
        .operation("fail", &ThrowingOperation::fail, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .check(spec);

    require(!result.success, "model checker should report throwing operation");
    require(result.failure == lincheck::FailureKind::unexpected_exception, "model checker exception should have unexpected-exception kind");
    require(result.message == "operation boom", "model checker exception should preserve formatted message");
    require_exception_message(result.exception, "operation boom", "model checker exception should preserve exception_ptr");
    require(result.trace.find("operation.start") != std::string::npos, "model checker exception trace should include operation start");
    require(result.trace.find("operation.throw") != std::string::npos, "model checker exception trace should include operation throw");
    require(result.trace.find("operation.throw thread=0 actor=0 fail() -> exception(") != std::string::npos, "model checker operation throw trace should include the exception value");
    require(result.trace.find("operation clocks:") != std::string::npos, "model checker exception trace should include operation clocks");
    require(result.trace.find("thread 0 actor 0 fail() -> exception(") != std::string::npos, "model checker operation clocks should include the throwing operation result");
    require(result.trace.find("parallel:") != std::string::npos, "model checker exception trace should include scenario");
}

void model_checker_aborts_peer_threads_after_primary_failure() {
    lincheck::TestSpec spec = lincheck::test<ThrowingOperation, SequentialCounter>()
        .operation("fail", &ThrowingOperation::fail, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "model checker should report the primary throwing operation");
    require(result.failure == lincheck::FailureKind::unexpected_exception, "primary throwing operation should remain the reported failure");
    require(result.message == "operation boom", "primary exception message should be preserved after aborting peers");
    require(result.trace.find("abort requested by thread 0") != std::string::npos, "model checker trace should record peer abort request");
    require(result.trace.find("thread 1 aborted") != std::string::npos, "model checker trace should record aborted peer thread");
}

void model_checker_deadlock_trace_includes_context_sections() {
    struct DeadlockingOperation {
        lincheck::mutex mutex;
        lincheck::condition_variable condition;

        int wait_forever() {
            lincheck::unique_lock lock(mutex);
            condition.wait(lock);
            return 0;
        }
    };

    struct SequentialDeadlockingOperation {
        int wait_forever() {
            return 0;
        }
    };

    lincheck::TestSpec spec = lincheck::test<DeadlockingOperation, SequentialDeadlockingOperation>()
        .operation("wait_forever", &DeadlockingOperation::wait_forever, &SequentialDeadlockingOperation::wait_forever);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(1)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(1)
        .minimize_failed_scenario(false)
        .check(spec);

    require(!result.success, "model checker should report managed condition-variable deadlock");
    require(result.failure == lincheck::FailureKind::deadlock, "deadlock should use the deadlock failure kind");
    require(result.trace.find("deadlock: all unfinished threads are blocked") != std::string::npos, "deadlock trace should explain blocking");
    require(result.trace.find("operation clocks:") != std::string::npos, "deadlock trace should include operation clocks");
    require(result.trace.find("parallel:") != std::string::npos, "deadlock trace should include scenario");
    require(result.trace.find("wait_forever()") != std::string::npos, "deadlock trace should name the blocked operation");
}

void model_checker_accepts_locked_counter() {
    lincheck::TestSpec spec = lincheck::test<LockedCounter, SequentialCounter>()
        .operation("inc", &LockedCounter::inc, &SequentialCounter::inc);

    const auto result = lincheck::ModelCheckingOptions()
        .iterations(1)
        .threads(2)
        .actors_per_thread(1)
        .actors_before(0)
        .actors_after(0)
        .max_schedule_length(4)
        .check(spec);

    require(result.success, "model checker should accept mutex-protected counter");
}

void scheduler_reports_lock_blocking_and_unblocking() {
    lincheck::CooperativeScheduler scheduler({0, 0, 1, 1, 0, 1, 1}, 2);
    lincheck::mutex mutex;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto worker = [&](int id) {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, id);
        try {
            scheduler.worker_ready(static_cast<std::size_t>(id));
            mutex.lock();
            lincheck::switch_point(id == 0 ? "worker0.inside" : "worker1.inside");
            mutex.unlock();
        } catch (...) {
            std::lock_guard lock(exception_mutex);
            if (!exception) exception = std::current_exception();
        }
        scheduler.finish_thread(static_cast<std::size_t>(id));
    };

    workers.emplace_back(worker, 0);
    workers.emplace_back(worker, 1);
    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker_thread : workers) worker_thread.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(trace.find("blocked on lock") != std::string::npos, "managed scheduler should report lock blocking");
    require(trace.find("unblocked on lock") != std::string::npos, "managed scheduler should report lock unblocking");
    require(trace.find("lock attempt obj#") != std::string::npos, "managed scheduler lock traces should use stable object IDs");
    require(trace.find("lock attempt 0x") == std::string::npos, "managed scheduler lock traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::mutex_lock_attempt),
        "managed scheduler should retain structured lock-attempt sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::mutex_lock_acquired),
        "managed scheduler should retain structured lock-acquired sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::mutex_unlock),
        "managed scheduler should retain structured unlock sync events"
    );
}

void scheduler_reports_timed_lock_wake_and_timeout() {
    {
        lincheck::CooperativeScheduler scheduler({0, 0, 0, 1, 1}, 2);
        lincheck::timed_mutex mutex;
        bool acquired = false;
        std::exception_ptr exception;
        std::mutex exception_mutex;
        std::vector<std::thread> workers;

        auto record_exception = [&] {
            std::lock_guard lock(exception_mutex);
            if (!exception) exception = std::current_exception();
        };

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
            try {
                scheduler.worker_ready(0);
                mutex.lock();
                lincheck::switch_point("timed_mutex.holder.inside");
                mutex.unlock();
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(0);
        });

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
            try {
                scheduler.worker_ready(1);
                acquired = mutex.try_lock_for(std::chrono::seconds(1));
                if (acquired) {
                    mutex.unlock();
                }
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(1);
        });

        scheduler.wait_until_ready();
        scheduler.start();
        for (auto& worker : workers) worker.join();

        if (exception) std::rethrow_exception(exception);

        const auto trace = scheduler.trace_string();
        require(acquired, "managed timed_mutex should wake when the owning thread unlocks");
        require(trace.find("waiting with timeout on lock") != std::string::npos, "trace should include timed lock wait");
        require(trace.find("unblocked on lock") != std::string::npos, "trace should include lock unblock");
        require(trace.find("timed lock woke on lock") != std::string::npos, "trace should include timed lock wake");
        require(trace.find("lock obj#") != std::string::npos, "timed lock traces should use stable object IDs");
    }

    {
        lincheck::CooperativeScheduler scheduler({0, 0, 0, 1}, 2);
        lincheck::timed_mutex mutex;
        bool acquired = true;
        std::exception_ptr exception;
        std::mutex exception_mutex;
        std::vector<std::thread> workers;

        auto record_exception = [&] {
            std::lock_guard lock(exception_mutex);
            if (!exception) exception = std::current_exception();
        };

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
            try {
                scheduler.worker_ready(0);
                mutex.lock();
                lincheck::switch_point("timed_mutex.holder.finished_locked");
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(0);
        });

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
            try {
                scheduler.worker_ready(1);
                acquired = mutex.try_lock_for(std::chrono::seconds(1));
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(1);
        });

        scheduler.wait_until_ready();
        scheduler.start();
        for (auto& worker : workers) worker.join();

        if (exception) std::rethrow_exception(exception);

        const auto trace = scheduler.trace_string();
        require(!acquired, "managed timed_mutex should time out when no runnable thread can unlock");
        require(trace.find("timed out on lock") != std::string::npos, "trace should include timed lock timeout");
        require(trace.find("timed lock expired on lock") != std::string::npos, "trace should include timed lock expiration");
    }
}

void scheduler_reports_shared_lock_wake_and_timeout() {
    {
        lincheck::CooperativeScheduler scheduler({0, 0, 1, 1, 0, 1, 1}, 2);
        lincheck::shared_mutex mutex;
        bool reader_entered = false;
        std::exception_ptr exception;
        std::mutex exception_mutex;
        std::vector<std::thread> workers;

        auto record_exception = [&] {
            std::lock_guard lock(exception_mutex);
            if (!exception) exception = std::current_exception();
        };

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
            try {
                scheduler.worker_ready(0);
                mutex.lock();
                lincheck::switch_point("shared_mutex.holder.inside");
                mutex.unlock();
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(0);
        });

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
            try {
                scheduler.worker_ready(1);
                mutex.lock_shared();
                reader_entered = true;
                lincheck::switch_point("shared_mutex.reader.inside");
                mutex.unlock_shared();
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(1);
        });

        scheduler.wait_until_ready();
        scheduler.start();
        for (auto& worker : workers) worker.join();

        if (exception) std::rethrow_exception(exception);

        const auto trace = scheduler.trace_string();
        require(reader_entered, "managed shared_mutex should wake a shared waiter after exclusive unlock");
        require(trace.find("blocked on shared lock") != std::string::npos, "trace should include shared lock blocking");
        require(trace.find("unblocked on shared lock") != std::string::npos, "trace should include shared lock unblocking");
        require(trace.find("shared lock attempt obj#") != std::string::npos, "shared lock traces should use stable object IDs");
        require(trace.find("shared lock attempt 0x") == std::string::npos, "shared lock traces should not expose raw addresses");
    }

    {
        lincheck::CooperativeScheduler scheduler({0, 0, 0, 1}, 2);
        lincheck::shared_timed_mutex mutex;
        bool acquired = true;
        std::exception_ptr exception;
        std::mutex exception_mutex;
        std::vector<std::thread> workers;

        auto record_exception = [&] {
            std::lock_guard lock(exception_mutex);
            if (!exception) exception = std::current_exception();
        };

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
            try {
                scheduler.worker_ready(0);
                mutex.lock();
                lincheck::switch_point("shared_timed_mutex.holder.finished_locked");
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(0);
        });

        workers.emplace_back([&] {
            lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
            try {
                scheduler.worker_ready(1);
                acquired = mutex.try_lock_shared_for(std::chrono::seconds(1));
                if (acquired) {
                    mutex.unlock_shared();
                }
            } catch (...) {
                record_exception();
            }
            scheduler.finish_thread(1);
        });

        scheduler.wait_until_ready();
        scheduler.start();
        for (auto& worker : workers) worker.join();

        if (exception) std::rethrow_exception(exception);

        const auto trace = scheduler.trace_string();
        require(!acquired, "managed shared_timed_mutex should time out when no runnable thread can unlock");
        require(trace.find("waiting with timeout on shared lock") != std::string::npos, "trace should include timed shared lock wait");
        require(trace.find("timed out on shared lock") != std::string::npos, "trace should include timed shared lock timeout");
        require(trace.find("timed shared lock expired on lock") != std::string::npos, "trace should include timed shared lock expiration");
    }
}

void scheduler_abort_wakes_waiting_threads() {
    lincheck::CooperativeScheduler scheduler({0}, 2);
    std::exception_ptr primary;
    std::exception_ptr unexpected;
    std::mutex exception_mutex;
    bool peer_aborted = false;
    std::vector<std::thread> workers;

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            lincheck::switch_point("primary.before_throw");
            throw std::runtime_error("primary failure");
        } catch (const lincheck::ScheduleAbortError&) {
        } catch (...) {
            {
                std::lock_guard lock(exception_mutex);
                if (!primary) primary = std::current_exception();
            }
            scheduler.request_abort(0);
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            lincheck::switch_point("peer.should_not_run");
        } catch (const lincheck::ScheduleAbortError&) {
            peer_aborted = true;
        } catch (...) {
            std::lock_guard lock(exception_mutex);
            if (!unexpected) unexpected = std::current_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (unexpected) std::rethrow_exception(unexpected);
    require_exception_message(primary, "primary failure", "scheduler abort should preserve primary failure");
    require(peer_aborted, "scheduler abort should wake and stop the waiting peer");

    const auto trace = scheduler.trace_string();
    require(trace.find("abort requested by thread 0") != std::string::npos, "scheduler trace should include abort request");
    require(trace.find("thread 1 aborted") != std::string::npos, "scheduler trace should include aborted peer");
    require(trace.find("peer.should_not_run") == std::string::npos, "aborted peer should not continue to its switch point");
}

void scheduler_basic_interleaving_matches_golden_trace() {
    lincheck::CooperativeScheduler scheduler({0, 1, 0, 1}, 2);
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto worker = [&](int id) {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, id);
        try {
            scheduler.worker_ready(static_cast<std::size_t>(id));
            lincheck::switch_point(id == 0 ? "t0.first" : "t1.first");
            lincheck::switch_point(id == 0 ? "t0.second" : "t1.second");
        } catch (...) {
            std::lock_guard lock(exception_mutex);
            if (!exception) exception = std::current_exception();
        }
        scheduler.finish_thread(static_cast<std::size_t>(id));
    };

    workers.emplace_back(worker, 0);
    workers.emplace_back(worker, 1);
    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker_thread : workers) worker_thread.join();

    if (exception) std::rethrow_exception(exception);

    const std::string expected =
        "schedule: 0 1 0 1 1\n"
        "schedule decisions:\n"
        "  #0 initial -> 0 runnable: 0 1\n"
        "  #1 thread 0 @ t0.first -> 1 runnable: 0 1\n"
        "  #2 thread 1 @ t1.first -> 0 runnable: 0 1\n"
        "  #3 thread 0 @ t0.second -> 1 runnable: 0 1\n"
        "  #4 thread 1 @ t1.second -> 1 runnable: 0 1\n"
        "initial -> thread 0\n"
        "thread 0 switch-point t0.first\n"
        "switch thread 0 -> 1\n"
        "thread 1 switch-point t1.first\n"
        "switch thread 1 -> 0\n"
        "thread 0 switch-point t0.second\n"
        "switch thread 0 -> 1\n"
        "thread 1 switch-point t1.second\n"
        "thread 1 finished\n"
        "thread 0 finished\n";

    require(scheduler.trace_string() == expected, "basic scheduler interleaving should match golden trace");
}

void condition_variable_wait_notify_works_on_real_threads() {
    lincheck::mutex mutex;
    lincheck::condition_variable condition;
    bool ready = false;
    int observed = 0;

    std::thread waiter([&] {
        lincheck::unique_lock lock(mutex);
        condition.wait(lock, [&] { return ready; });
        observed = 1;
    });

    {
        lincheck::unique_lock lock(mutex);
        ready = true;
    }
    condition.notify_one();
    waiter.join();

    require(observed == 1, "condition_variable should wake a real waiting thread");
}

void condition_variable_timed_wait_times_out_on_real_threads() {
    lincheck::mutex mutex;
    lincheck::condition_variable condition;
    bool ready = false;

    lincheck::unique_lock lock(mutex);
    const auto status = condition.wait_for(lock, std::chrono::milliseconds(0));
    require(status == std::cv_status::timeout, "condition_variable wait_for should time out on real threads");
    require(lock.owns_lock(), "condition_variable wait_for should reacquire the lock after timeout");

    const auto predicate_result = condition.wait_until(lock, std::chrono::steady_clock::now(), [&] {
        return ready;
    });
    require(!predicate_result, "condition_variable wait_until predicate overload should report timeout");
    require(lock.owns_lock(), "condition_variable wait_until should leave the lock owned");
}

void parker_park_unpark_works_on_real_threads() {
    lincheck::parker parker;
    std::atomic<bool> started{false};
    std::atomic<bool> resumed{false};

    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        parker.park();
        resumed.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    parker.unpark();
    waiter.join();

    require(resumed.load(std::memory_order_acquire), "parker should wake a real parked thread");
}

void atomic_wait_notify_works_on_real_threads() {
    lincheck::atomic<int> flag{0};
    std::atomic<bool> started{false};
    std::atomic<int> observed{0};

    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        flag.wait(0);
        observed.store(flag.load(), std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    flag.store(1);
    flag.notify_one();
    waiter.join();

    require(observed.load(std::memory_order_acquire) == 1, "atomic wait should wake after notify when the value changes");
}

void semaphore_acquire_release_works_on_real_threads() {
    lincheck::binary_semaphore semaphore{0};
    std::atomic<bool> started{false};
    std::atomic<bool> resumed{false};

    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        semaphore.acquire();
        resumed.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    require(!semaphore.try_acquire(), "binary semaphore should have no spare permit while waiter is blocked");
    semaphore.release();
    waiter.join();

    require(resumed.load(std::memory_order_acquire), "semaphore should wake a real waiting thread");

    lincheck::counting_semaphore<3> permits{2};
    require(permits.try_acquire(), "counting semaphore should acquire the first initial permit");
    require(permits.try_acquire(), "counting semaphore should acquire the second initial permit");
    require(!permits.try_acquire_for(std::chrono::milliseconds(0)), "empty counting semaphore should time out");
    permits.release(2);
    require(permits.try_acquire_until(std::chrono::steady_clock::now()), "released counting semaphore permit should be visible");
}

void latch_wait_count_down_works_on_real_threads() {
    lincheck::latch latch{1};
    std::atomic<bool> started{false};
    std::atomic<bool> resumed{false};

    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        latch.wait();
        resumed.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    require(!latch.try_wait(), "latch should not be ready before count_down");
    latch.count_down();
    waiter.join();

    require(resumed.load(std::memory_order_acquire), "latch should wake a real waiting thread");
    require(latch.try_wait(), "latch should report ready after count reaches zero");

    lincheck::latch already_ready{0};
    already_ready.arrive_and_wait(0);
}

void barrier_arrive_and_wait_works_on_real_threads() {
    lincheck::barrier<> phase{2};
    std::atomic<int> before{0};
    std::atomic<int> after_first_phase{0};
    std::atomic<int> after_second_phase{0};
    std::atomic<int> first_phase_violations{0};
    std::atomic<int> second_phase_violations{0};

    auto worker = [&] {
        before.fetch_add(1, std::memory_order_acq_rel);
        phase.arrive_and_wait();
        if (before.load(std::memory_order_acquire) != 2) {
            first_phase_violations.fetch_add(1, std::memory_order_acq_rel);
        }
        after_first_phase.fetch_add(1, std::memory_order_acq_rel);
        phase.arrive_and_wait();
        if (after_first_phase.load(std::memory_order_acquire) != 2) {
            second_phase_violations.fetch_add(1, std::memory_order_acq_rel);
        }
        after_second_phase.fetch_add(1, std::memory_order_acq_rel);
    };

    std::thread left(worker);
    std::thread right(worker);
    left.join();
    right.join();

    require(first_phase_violations.load(std::memory_order_acquire) == 0, "barrier should wait for both real threads");
    require(second_phase_violations.load(std::memory_order_acquire) == 0, "barrier should be reusable for a second phase");
    require(after_second_phase.load(std::memory_order_acquire) == 2, "both real threads should pass both barrier phases");

    lincheck::barrier<> single{1};
    auto token = single.arrive();
    single.wait(std::move(token));
    single.arrive_and_drop();
}

void scheduler_reports_condition_variable_timed_wait_timeout() {
    lincheck::CooperativeScheduler scheduler({0}, 1);
    lincheck::mutex mutex;
    lincheck::condition_variable condition;
    std::cv_status observed = std::cv_status::no_timeout;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::thread worker([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            lincheck::unique_lock lock(mutex);
            observed = condition.wait_for(lock, std::chrono::seconds(1));
        } catch (...) {
            std::lock_guard guard(exception_mutex);
            if (!exception) exception = std::current_exception();
        }
        scheduler.finish_thread(0);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed == std::cv_status::timeout, "managed timed wait should report timeout when no runnable thread remains");
    require(trace.find("waiting with timeout on condition_variable") != std::string::npos, "trace should include timed condition wait");
    require(trace.find("timed out on condition_variable") != std::string::npos, "trace should include timed wait timeout");
    require(trace.find("timed wait expired on condition_variable") != std::string::npos, "trace should include timed wait expiration");
    require(trace.find("condition_variable obj#") != std::string::npos, "condition_variable traces should use stable object IDs");
    require(trace.find("condition_variable 0x") == std::string::npos, "condition_variable traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::condition_wait),
        "managed timed waits should retain structured condition wait sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::condition_wake),
        "managed timed waits should retain structured condition wake sync events"
    );
}

void scheduler_reports_park_and_unpark() {
    lincheck::CooperativeScheduler scheduler({0, 0, 0}, 2);
    lincheck::parker parker;
    int observed = 0;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto record_exception = [&] {
        std::lock_guard lock(exception_mutex);
        if (!exception) exception = std::current_exception();
    };

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            parker.park();
            observed = 1;
            lincheck::switch_point("parked.after_resume");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            parker.unpark();
            lincheck::switch_point("unparker.after_unpark");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed == 1, "managed parker should resume the parked thread");
    require(trace.find("parked ") != std::string::npos, "trace should include park blocking");
    require(trace.find("unpark ") != std::string::npos, "trace should include unpark operation");
    require(trace.find("unparked ") != std::string::npos, "trace should include unparked waiter");
    require(trace.find("resumed from park") != std::string::npos, "trace should include park resume");
    require(trace.find("park obj#") != std::string::npos, "parker traces should use stable object IDs");
    require(trace.find("park 0x") == std::string::npos, "parker traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::parker_park),
        "managed parker should retain structured park sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::parker_unpark),
        "managed parker should retain structured unpark sync events"
    );
}

void scheduler_reports_atomic_wait_and_notify() {
    lincheck::CooperativeScheduler scheduler({0, 1, 0, 0, 0, 0, 1}, 2);
    lincheck::atomic<int> flag{0};
    int observed = 0;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto record_exception = [&] {
        std::lock_guard lock(exception_mutex);
        if (!exception) exception = std::current_exception();
    };

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            flag.wait(0);
            observed = flag.load();
            lincheck::switch_point("atomic.waiter.after_wait");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            flag.store(1);
            flag.notify_one();
            lincheck::switch_point("atomic.notifier.after_notify");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed == 1, "managed atomic wait should resume after notify and value change");
    require(trace.find("waiting on atomic") != std::string::npos, "trace should include atomic wait blocking");
    require(trace.find("notify_one atomic") != std::string::npos, "trace should include atomic notify");
    require(trace.find("notified on atomic") != std::string::npos, "trace should include notified atomic waiter");
    require(trace.find("woke on atomic") != std::string::npos, "trace should include atomic waiter wake");
    require(trace.find("atomic obj#") != std::string::npos, "atomic wait traces should use stable object IDs");
    require(trace.find("atomic 0x") == std::string::npos, "atomic wait traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::atomic_wait),
        "managed atomic waits should retain structured atomic wait sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::atomic_wake),
        "managed atomic waits should retain structured atomic wake sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::atomic_notify_one),
        "managed atomic waits should retain structured atomic notify sync events"
    );
}

void scheduler_reports_semaphore_acquire_and_release() {
    lincheck::CooperativeScheduler scheduler({0, 1, 1, 0}, 2);
    lincheck::binary_semaphore semaphore{0};
    int observed = 0;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto record_exception = [&] {
        std::lock_guard lock(exception_mutex);
        if (!exception) exception = std::current_exception();
    };

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            semaphore.acquire();
            observed = 1;
            lincheck::switch_point("semaphore.waiter.after_acquire");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            semaphore.release();
            lincheck::switch_point("semaphore.releaser.after_release");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed == 1, "managed semaphore should resume the acquiring thread after release");
    require(trace.find("blocked on semaphore") != std::string::npos, "trace should include semaphore blocking");
    require(trace.find("release semaphore") != std::string::npos, "trace should include semaphore release");
    require(trace.find("unblocked on semaphore") != std::string::npos, "trace should include semaphore unblock");
    require(trace.find("woke on semaphore") != std::string::npos, "trace should include semaphore wake");
    require(trace.find("semaphore obj#") != std::string::npos, "semaphore traces should use stable object IDs");
    require(trace.find("semaphore 0x") == std::string::npos, "semaphore traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::semaphore_acquire),
        "managed semaphores should retain structured acquire sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::semaphore_release),
        "managed semaphores should retain structured release sync events"
    );
}

void scheduler_reports_semaphore_timed_acquire_timeout() {
    lincheck::CooperativeScheduler scheduler({0}, 1);
    lincheck::binary_semaphore semaphore{0};
    bool acquired = true;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::thread worker([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            acquired = semaphore.try_acquire_for(std::chrono::seconds(1));
        } catch (...) {
            std::lock_guard guard(exception_mutex);
            if (!exception) exception = std::current_exception();
        }
        scheduler.finish_thread(0);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(!acquired, "managed timed semaphore acquire should time out when no runnable thread remains");
    require(trace.find("waiting with timeout on semaphore") != std::string::npos, "trace should include timed semaphore wait");
    require(trace.find("timed out on semaphore") != std::string::npos, "trace should include timed semaphore timeout");
    require(trace.find("timed acquire expired on semaphore") != std::string::npos, "trace should include timed acquire expiration");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::semaphore_try_acquire),
        "managed timed semaphores should retain structured try_acquire sync events"
    );
}

void scheduler_reports_latch_wait_and_count_down() {
    lincheck::CooperativeScheduler scheduler({0, 1, 1, 0}, 2);
    lincheck::latch latch{1};
    int observed = 0;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto record_exception = [&] {
        std::lock_guard lock(exception_mutex);
        if (!exception) exception = std::current_exception();
    };

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            latch.wait();
            observed = 1;
            lincheck::switch_point("latch.waiter.after_wait");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            latch.count_down();
            lincheck::switch_point("latch.counter.after_count_down");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed == 1, "managed latch should resume the waiting thread after count_down");
    require(trace.find("blocked on latch") != std::string::npos, "trace should include latch blocking");
    require(trace.find("count_down latch") != std::string::npos, "trace should include latch count_down");
    require(trace.find("unblocked on latch") != std::string::npos, "trace should include latch unblock");
    require(trace.find("woke on latch") != std::string::npos, "trace should include latch wake");
    require(trace.find("latch obj#") != std::string::npos, "latch traces should use stable object IDs");
    require(trace.find("latch 0x") == std::string::npos, "latch traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::latch_wait),
        "managed latches should retain structured wait sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::latch_wake),
        "managed latches should retain structured wake sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::latch_count_down),
        "managed latches should retain structured count_down sync events"
    );
}

void scheduler_reports_barrier_wait_and_phase_completion() {
    lincheck::CooperativeScheduler scheduler({0, 0, 0, 1, 1}, 2);
    lincheck::barrier<> barrier{2};
    std::atomic<int> observed{0};
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto record_exception = [&] {
        std::lock_guard lock(exception_mutex);
        if (!exception) exception = std::current_exception();
    };

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            barrier.arrive_and_wait();
            observed.fetch_add(1, std::memory_order_acq_rel);
            lincheck::switch_point("barrier.left.after_wait");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            barrier.arrive_and_wait();
            observed.fetch_add(1, std::memory_order_acq_rel);
            lincheck::switch_point("barrier.right.after_wait");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed.load(std::memory_order_acquire) == 2, "managed barrier should resume both waiting threads");
    require(trace.find("arrive barrier") != std::string::npos, "trace should include barrier arrival");
    require(trace.find("blocked on barrier") != std::string::npos, "trace should include barrier blocking");
    require(trace.find("phase 0 complete") != std::string::npos, "trace should include barrier phase completion");
    require(trace.find("unblocked on barrier") != std::string::npos, "trace should include barrier unblock");
    require(trace.find("woke on barrier") != std::string::npos, "trace should include barrier wake");
    require(trace.find("barrier obj#") != std::string::npos, "barrier traces should use stable object IDs");
    require(trace.find("barrier 0x") == std::string::npos, "barrier traces should not expose raw addresses");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::barrier_arrive),
        "managed barriers should retain structured arrive sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::barrier_wait),
        "managed barriers should retain structured wait sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::barrier_wake),
        "managed barriers should retain structured wake sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::barrier_phase_complete),
        "managed barriers should retain structured phase-complete sync events"
    );
}

void scheduler_reports_condition_variable_wait_and_notify() {
    lincheck::CooperativeScheduler scheduler({0, 0, 0, 1, 1, 0, 1, 0, 0}, 2);
    lincheck::mutex mutex;
    lincheck::condition_variable condition;
    bool ready = false;
    int observed = 0;
    std::exception_ptr exception;
    std::mutex exception_mutex;
    std::vector<std::thread> workers;

    auto record_exception = [&] {
        std::lock_guard lock(exception_mutex);
        if (!exception) exception = std::current_exception();
    };

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 0);
        try {
            scheduler.worker_ready(0);
            lincheck::unique_lock lock(mutex);
            while (!ready) {
                condition.wait(lock);
            }
            observed = 1;
            lincheck::switch_point("waiter.after_wait");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(0);
    });

    workers.emplace_back([&] {
        lincheck::CooperativeScheduler::ThreadScope scope(scheduler, 1);
        try {
            scheduler.worker_ready(1);
            lincheck::unique_lock lock(mutex);
            ready = true;
            condition.notify_one();
            lincheck::switch_point("notifier.after_notify");
        } catch (...) {
            record_exception();
        }
        scheduler.finish_thread(1);
    });

    scheduler.wait_until_ready();
    scheduler.start();
    for (auto& worker : workers) worker.join();

    if (exception) std::rethrow_exception(exception);

    const auto trace = scheduler.trace_string();
    require(observed == 1, "managed condition_variable should wake the waiter");
    require(trace.find("waiting on condition_variable") != std::string::npos, "trace should include condition wait");
    require(trace.find("notify_one condition_variable") != std::string::npos, "trace should include condition notify");
    require(trace.find("notified on condition_variable") != std::string::npos, "trace should include notified waiter");
    require(trace.find("woke on condition_variable") != std::string::npos, "trace should include waiter wake");
    require(trace.find("condition_variable obj#") != std::string::npos, "condition_variable wait traces should use stable object IDs");
    const auto sync_events = scheduler.synchronization_events();
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::condition_wait),
        "managed condition waits should retain structured condition wait sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::condition_wake),
        "managed condition waits should retain structured condition wake sync events"
    );
    require(
        has_synchronization_event(sync_events, lincheck::SynchronizationEventKind::condition_notify_one),
        "managed condition waits should retain structured condition notify sync events"
    );
}

void thread_wrapper_propagates_runtime_callbacks() {
    RecordingRuntime runtime;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::thread worker([] {
            lincheck::switch_point("child.switch");
        });
        worker.join();
    }

    const auto has_child_switch = std::find(runtime.switches.begin(), runtime.switches.end(), "child.switch") != runtime.switches.end();
    const auto has_start = std::find(runtime.events.begin(), runtime.events.end(), "thread.start") != runtime.events.end();
    require(has_child_switch, "lincheck::thread should propagate runtime switch callbacks");
    require(has_start, "lincheck::thread should emit runtime events");
}

void thread_wrapper_supports_arguments_joinable_id_and_detach() {
    RecordingRuntime runtime;
    int value = 0;
    std::atomic<bool> saw_this_thread_id{false};
    std::atomic<bool> release_detached{false};

    auto finish_count = [&] {
        std::lock_guard lock(runtime.mutex);
        return std::count(runtime.events.begin(), runtime.events.end(), "thread.finish");
    };

    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::thread worker([&](int delta, int& target) {
            target += delta;
            saw_this_thread_id.store(lincheck::this_thread::get_id() != lincheck::thread::id{}, std::memory_order_release);
            lincheck::this_thread::yield();
            lincheck::switch_point("argument.thread");
        }, 7, std::ref(value));

        require(worker.joinable(), "argument thread should be joinable after construction");
        require(worker.get_id() != lincheck::thread::id{}, "argument thread should expose a native thread id");
        worker.join();
        require(!worker.joinable(), "joined thread should no longer be joinable");
        require(value == 7, "lincheck::thread should pass constructor arguments to the child callable");

        std::atomic<int> swapped_sum{0};
        lincheck::thread first_swap([&] {
            swapped_sum.fetch_add(1, std::memory_order_relaxed);
        });
        lincheck::thread second_swap([&] {
            swapped_sum.fetch_add(10, std::memory_order_relaxed);
        });
        (void)first_swap.native_handle();
        const auto first_swap_id = first_swap.get_id();
        const auto second_swap_id = second_swap.get_id();
        first_swap.swap(second_swap);
        require(first_swap.get_id() == second_swap_id, "lincheck::thread member swap should exchange native thread IDs");
        require(second_swap.get_id() == first_swap_id, "lincheck::thread member swap should exchange the other native thread ID");
        swap(first_swap, second_swap);
        require(first_swap.get_id() == first_swap_id, "lincheck::thread ADL swap should restore the first native thread ID");
        require(second_swap.get_id() == second_swap_id, "lincheck::thread ADL swap should restore the second native thread ID");
        first_swap.join();
        second_swap.join();
        require(swapped_sum.load(std::memory_order_relaxed) == 11, "swapped lincheck::thread instances should remain joinable");

        const auto finishes_before_detach = finish_count();
        lincheck::thread detached([&] {
            while (!release_detached.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            lincheck::switch_point("detached.thread");
        });
        require(detached.joinable(), "detached candidate should be joinable before detach");
        detached.detach();
        require(!detached.joinable(), "detached thread should no longer be joinable");
        release_detached.store(true, std::memory_order_release);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (finish_count() < finishes_before_detach + 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    }

    auto has_switch = [&](const std::string& needle) {
        return std::find(runtime.switches.begin(), runtime.switches.end(), needle) != runtime.switches.end();
    };
    auto has_event = [&](const std::string& needle) {
        return std::find(runtime.events.begin(), runtime.events.end(), needle) != runtime.events.end();
    };

    require(value == 7, "joined argument thread should update the referenced target");
    require(saw_this_thread_id.load(std::memory_order_acquire), "lincheck::this_thread::get_id should expose the child thread id");
    require(finish_count() >= 2, "detached thread should finish before the test leaves the inherited runtime scope");
    require(has_switch("yield"), "lincheck::this_thread::yield should use the runtime-aware yield switch point");
    require(has_switch("argument.thread"), "argument thread should inherit runtime switch callbacks");
    require(has_switch("detached.thread"), "detached thread should inherit runtime switch callbacks");
    require(has_event("thread.detach"), "lincheck::thread detach should emit a runtime event");
}

void thread_wrapper_rethrows_child_exception_on_join() {
    RecordingRuntime runtime;
    bool caught = false;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::thread worker([] {
            throw std::runtime_error("child boom");
        });
        try {
            worker.join();
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "child boom";
        }
    }

    const auto has_exception = std::find(runtime.events.begin(), runtime.events.end(), "thread.exception") != runtime.events.end();
    const auto has_finish = std::find(runtime.events.begin(), runtime.events.end(), "thread.finish") != runtime.events.end();

    require(caught, "lincheck::thread join should rethrow child exception");
    require(has_exception, "lincheck::thread should trace child exception");
    require(has_finish, "lincheck::thread should trace finish after child exception");
}

void jthread_wrapper_supports_stop_token_runtime_and_auto_join() {
    RecordingRuntime runtime;
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    int value = 0;

    {
        lincheck::ScopedRuntime scoped(&runtime);
        {
            lincheck::jthread worker([&](std::stop_token token, int delta) {
                value += delta;
                started.store(true, std::memory_order_release);
                while (!token.stop_requested()) {
                    lincheck::this_thread::yield();
                }
                stopped.store(true, std::memory_order_release);
                lincheck::switch_point("jthread.stop");
            }, 9);

            require(worker.joinable(), "lincheck::jthread should be joinable after construction");
            require(worker.get_id() != lincheck::jthread::id{}, "lincheck::jthread should expose a native thread id");
            require(worker.stop_possible(), "lincheck::jthread should expose a stoppable token");
            (void)worker.get_stop_source();
            (void)worker.get_stop_token();
            (void)worker.native_handle();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            require(started.load(std::memory_order_acquire), "lincheck::jthread worker should start before scope exit");
        }
    }

    auto has_switch = [&](const std::string& needle) {
        return std::find(runtime.switches.begin(), runtime.switches.end(), needle) != runtime.switches.end();
    };
    auto has_event = [&](const std::string& needle) {
        return std::find(runtime.events.begin(), runtime.events.end(), needle) != runtime.events.end();
    };

    require(value == 9, "lincheck::jthread should pass constructor arguments to the child callable");
    require(stopped.load(std::memory_order_acquire), "lincheck::jthread destructor should request stop and join");
    require(has_switch("jthread.stop"), "lincheck::jthread should propagate runtime switch callbacks");
    require(has_event("jthread.start"), "lincheck::jthread should emit start events");
    require(has_event("jthread.finish"), "lincheck::jthread should emit finish events");
}

void jthread_wrapper_rethrows_child_exception_on_join() {
    RecordingRuntime runtime;
    bool caught = false;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::jthread worker([] {
            throw std::runtime_error("jthread child boom");
        });
        try {
            worker.join();
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "jthread child boom";
        }
    }

    const auto has_exception = std::find(runtime.events.begin(), runtime.events.end(), "jthread.exception") != runtime.events.end();
    const auto has_finish = std::find(runtime.events.begin(), runtime.events.end(), "jthread.finish") != runtime.events.end();

    require(caught, "lincheck::jthread join should rethrow child exception");
    require(has_exception, "lincheck::jthread should trace child exception");
    require(has_finish, "lincheck::jthread should trace finish after child exception");
}

void this_thread_sleep_wrappers_emit_trace_and_switch_points() {
    RecordingRuntime runtime;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::this_thread::sleep_for(std::chrono::nanoseconds(0));
        lincheck::this_thread::sleep_until(std::chrono::steady_clock::now());
    }

    auto has_event = [&](const std::string& needle) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };
    auto has_switch = [&](const std::string& needle) {
        return std::find(runtime.switches.begin(), runtime.switches.end(), needle) != runtime.switches.end();
    };

    require(has_event("this_thread.sleep_for duration_ns=0"), "sleep_for wrapper should emit a trace event");
    require(has_event("this_thread.sleep_until"), "sleep_until wrapper should emit a trace event");
    require(has_switch("this_thread.sleep_for"), "sleep_for wrapper should emit a switch point");
    require(has_switch("this_thread.sleep_until"), "sleep_until wrapper should emit a switch point");
}

void run_concurrent_test_records_threaded_block_trace() {
    lincheck::atomic<int> counter{0};
    const auto result = lincheck::run_concurrent_test([&] {
        lincheck::thread first([&] {
            counter.fetch_add(1);
        });
        lincheck::thread second([&] {
            counter.fetch_add(1);
        });
        first.join();
        second.join();
        if (counter.load() != 2) {
            throw std::runtime_error("counter mismatch");
        }
    });

    require(result.success, "run_concurrent_test should accept a successful concurrent block");
    require(result.trace.find("concurrent test trace:") != std::string::npos, "concurrent block trace should have a header");
    require(result.trace.find("concurrent_test.start") != std::string::npos, "concurrent block trace should include start event");
    require(result.trace.find("thread.start") != std::string::npos, "concurrent block trace should include child thread start");
    require(result.trace.find("atomic.fetch_add") != std::string::npos, "concurrent block trace should include atomic operations");
    require(result.trace.find("thread.join") != std::string::npos, "concurrent block trace should include joins");
    require(
        std::any_of(result.trace_events.begin(), result.trace_events.end(), [](const auto& record) {
            return record.kind == "thread.start" &&
                record.thread_id >= 0 &&
                lincheck::to_string(record).find("thread.start") != std::string::npos;
        }),
        "concurrent block result should retain structured thread trace events"
    );
}

void run_concurrent_test_reports_non_seq_cst_memory_order_warning() {
    lincheck::atomic<int> counter{0};
    const auto result = lincheck::run_concurrent_test([&] {
        lincheck::thread worker([&] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        worker.join();
    });

    require(result.success, "run_concurrent_test should accept relaxed atomic block");
    require(
        has_warning(result, "atomic.fetch_add", "sequentially consistent"),
        "run_concurrent_test should return non-seq_cst warning from child thread"
    );
    require(
        has_memory_event(result, lincheck::MemoryEventKind::atomic_fetch_add),
        "run_concurrent_test should expose structured memory events"
    );
    require(
        !result.memory_events.empty() &&
            lincheck::to_string(result.memory_events.front()).find("thread=") != std::string::npos,
        "memory event records should format sequence and thread metadata"
    );
    require(
        has_memory_event_object_id(result, lincheck::MemoryEventKind::atomic_fetch_add),
        "run_concurrent_test memory events should retain stable object IDs"
    );
}

void run_concurrent_test_retains_source_metadata_for_fence_events() {
    const auto result = lincheck::run_concurrent_test([&] {
        LC_THREAD_FENCE(std::memory_order_seq_cst);
        LC_SIGNAL_FENCE(std::memory_order_seq_cst);
    });

    require(result.success, "run_concurrent_test should accept source-location fence helpers");
    require(
        has_memory_event_source_location(result, lincheck::MemoryEventKind::atomic_thread_fence),
        "run_concurrent_test should retain source metadata for thread-fence memory events"
    );
    require(
        has_memory_event_source_location(result, lincheck::MemoryEventKind::atomic_signal_fence),
        "run_concurrent_test should retain source metadata for signal-fence memory events"
    );
}

void run_concurrent_test_records_park_unpark_trace() {
    lincheck::parker parker;
    const auto result = lincheck::run_concurrent_test([&] {
        parker.unpark();
        lincheck::thread worker([&] {
            parker.park();
        });
        worker.join();
    });

    require(result.success, "run_concurrent_test should accept park/unpark block");
    require(result.trace.find("parker.unpark") != std::string::npos, "concurrent trace should include unpark");
    require(result.trace.find("parker.park") != std::string::npos, "concurrent trace should include park");
    require(
        has_synchronization_event(result, lincheck::SynchronizationEventKind::parker_park),
        "run_concurrent_test should retain structured park sync events"
    );
    require(
        has_synchronization_event(result, lincheck::SynchronizationEventKind::parker_unpark),
        "run_concurrent_test should retain structured unpark sync events"
    );
    require(
        has_event_dependency_node(result, "synchronization", "sync.parker.park"),
        "run_concurrent_test should expose synchronization dependency nodes"
    );
    require(
        has_event_dependency_edge(result, lincheck::EventDependencyEdgeKind::stream_resource_order, "obj#"),
        "run_concurrent_test should expose synchronization same-object dependency edges"
    );
}

void run_concurrent_test_applies_trace_filter() {
    lincheck::atomic<int> counter{0};
    lincheck::TraceFilter filter;
    filter.exclude("thread.start");

    const auto result = lincheck::run_concurrent_test([&] {
        lincheck::thread worker([&] {
            counter.fetch_add(1);
        });
        worker.join();
    }, filter);

    require(result.success, "run_concurrent_test should still execute when trace lines are filtered");
    require(result.trace.find("thread.start") == std::string::npos, "concurrent block filter should suppress matching trace lines");
    require(result.trace.find("atomic.fetch_add") != std::string::npos, "concurrent block filter should retain non-matching trace lines");
}

void run_concurrent_test_preserves_child_exception() {
    const auto result = lincheck::run_concurrent_test([] {
        LC_THREAD_FENCE(std::memory_order_seq_cst);
        lincheck::thread worker([] {
            throw std::runtime_error("concurrent child boom");
        });
        worker.join();
    });

    require(!result.success, "run_concurrent_test should report child exception");
    require(result.failure == lincheck::FailureKind::unexpected_exception, "concurrent block exception should be unexpected-exception kind");
    require(result.message == "concurrent child boom", "concurrent block exception should preserve formatted message");
    require_exception_message(result.exception, "concurrent child boom", "concurrent block should preserve exception_ptr");
    require(result.trace.find("failure:\n  kind: unexpected_exception") != std::string::npos, "concurrent block trace should include failure summary");
    require(result.trace.find("memory events:\n") != std::string::npos, "concurrent block failure trace should include memory events");
    require(result.trace.find("atomic_thread_fence") != std::string::npos, "memory event trace should include the fenced operation");
    require(result.trace.find("location=loc#") != std::string::npos, "memory event trace should include stable location IDs");
    require(result.trace.find("thread.exception") != std::string::npos, "concurrent block trace should include child exception event");
}

void source_macros_emit_location_metadata() {
    RecordingRuntime runtime;
    int value = 1;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        const int observed = LC_READ(value);
        const int called = LC_CALL("increment-helper", [&] {
            return observed + 10;
        });
        int side_effect = 0;
        LC_CALL("void-helper", [&] {
            side_effect = called + 1;
        });
        bool threw = false;
        try {
            LC_CALL("throwing-helper", []() -> int {
                throw std::runtime_error("call failed");
            });
        } catch (const std::runtime_error&) {
            threw = true;
        }
        LC_WRITE(value, observed + 1);
        LC_SWITCH();
        LC_YIELD();
        lincheck::yield("named.yield");
        require(called == 11, "LC_CALL should return callable results");
        require(side_effect == 12, "LC_CALL should support void callables");
        require(threw, "LC_CALL should preserve callable exceptions");
    }

    const auto has_read_event = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("source.read") != std::string::npos &&
            event.find("obj#") != std::string::npos &&
            event.find("loc#") != std::string::npos &&
            event.find("0x") == std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos &&
            event.find("source_macros_emit_location_metadata") != std::string::npos;
    });
    const auto has_write_event = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("source.write") != std::string::npos &&
            event.find("obj#") != std::string::npos &&
            event.find("loc#") != std::string::npos &&
            event.find("0x") == std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_manual_switch = std::any_of(runtime.switches.begin(), runtime.switches.end(), [](const std::string& event) {
        return event.find("manual @") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_macro_yield = std::any_of(runtime.switches.begin(), runtime.switches.end(), [](const std::string& event) {
        return event.find("yield @") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_call_begin_event = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("call.begin increment-helper") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos &&
            event.find("source_macros_emit_location_metadata") != std::string::npos;
    });
    const auto has_call_end_event = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("call.end increment-helper") != std::string::npos &&
            event.find("-> 11") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_void_call_end_event = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("call.end void-helper") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_call_throw_event = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("call.throw throwing-helper") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_call_begin_switch = std::any_of(runtime.switches.begin(), runtime.switches.end(), [](const std::string& event) {
        return event.find("call.begin @") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_call_end_switch = std::any_of(runtime.switches.begin(), runtime.switches.end(), [](const std::string& event) {
        return event.find("call.end @") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_call_throw_switch = std::any_of(runtime.switches.begin(), runtime.switches.end(), [](const std::string& event) {
        return event.find("call.throw @") != std::string::npos &&
            event.find("lincheck_mvp_tests.cpp") != std::string::npos;
    });
    const auto has_named_yield = std::find(runtime.switches.begin(), runtime.switches.end(), "named.yield") != runtime.switches.end();

    require(value == 2, "LC_WRITE should update the target");
    require(has_read_event, "LC_READ should emit source-location metadata and stable IDs");
    require(has_write_event, "LC_WRITE should emit source-location metadata and stable IDs");
    require(has_call_begin_event, "LC_CALL should emit begin source-location metadata");
    require(has_call_end_event, "LC_CALL should emit end source-location metadata");
    require(has_void_call_end_event, "LC_CALL should emit end events for void callables");
    require(has_call_throw_event, "LC_CALL should emit throw source-location metadata");
    require(has_call_begin_switch, "LC_CALL should emit a begin switch point");
    require(has_call_end_switch, "LC_CALL should emit an end switch point");
    require(has_call_throw_switch, "LC_CALL should emit a throw switch point");
    require(has_manual_switch, "LC_SWITCH should emit source-location metadata");
    require(has_macro_yield, "LC_YIELD should emit source-location metadata");
    require(has_named_yield, "lincheck::yield should emit a named switch point");
    require(
        std::any_of(runtime.source_accesses.begin(), runtime.source_accesses.end(), [](const auto& event) {
            return event.kind == lincheck::SourceAccessKind::read &&
                event.object != nullptr &&
                event.has_value &&
                lincheck::value_cast<int>(event.value) == 1 &&
                event.source.file != nullptr &&
                std::string(event.source.file).find("lincheck_mvp_tests.cpp") != std::string::npos;
        }),
        "LC_READ should emit structured source access metadata"
    );
    require(
        std::any_of(runtime.source_accesses.begin(), runtime.source_accesses.end(), [](const auto& event) {
            return event.kind == lincheck::SourceAccessKind::write &&
                event.object != nullptr &&
                event.has_value &&
                lincheck::value_cast<int>(event.value) == 2;
        }),
        "LC_WRITE should emit structured source access metadata"
    );
}

void source_audit_detects_raw_sync_and_filters_noise() {
    const std::string source = R"cpp(
#include <atomic>
struct Example {
    std::atomic<int> value{0};
    std::atomic_ref<int> value_ref;
    std::mutex suppressed; // NOLINT(lincheck-raw-sync)
    // NOLINTNEXTLINE(lincheck-raw-sync)
    std::thread allowed_worker;
    std::jthread service;
    std::condition_variable cv;
    std::barrier<> phase{2};
    std::latch gate{1};
    std::binary_semaphore permit{0};
    std::recursive_mutex recursive;
    std::timed_mutex timed;
    std::recursive_timed_mutex recursive_timed;
    std::shared_mutex shared;
    std::shared_timed_mutex shared_timed;
    std::shared_lock<std::shared_mutex> shared_guard;
    const char* text = "std::jthread in a string literal";
    // std::recursive_mutex in a comment
    void bump(int* ptr) {
        __atomic_fetch_add(ptr, 1, __ATOMIC_SEQ_CST);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};
)cpp";

    auto findings = lincheck::audit_source_text("src/example.cpp", source);
    require(findings.size() == 16, "source audit should report unsuppressed raw synchronization APIs");

    const auto has_atomic = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_atomic && finding.token == "std::atomic";
    });
    const auto has_atomic_ref = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_atomic && finding.token == "std::atomic_ref";
    });
    const auto has_condition = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_condition_variable &&
            finding.token == "std::condition_variable";
    });
    const auto has_jthread = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_thread && finding.token == "std::jthread";
    });
    const auto has_builtin = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::compiler_atomic_builtin && finding.token == "__atomic_";
    });
    const auto has_sleep = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_wait && finding.token == "std::this_thread::sleep_for";
    });
    const auto has_barrier = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_wait && finding.token == "std::barrier";
    });
    const auto has_latch = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_wait && finding.token == "std::latch";
    });
    const auto has_semaphore = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_wait && finding.token == "std::binary_semaphore";
    });
    const auto has_recursive_mutex = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_mutex && finding.token == "std::recursive_mutex";
    });
    const auto has_timed_mutex = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_mutex && finding.token == "std::timed_mutex";
    });
    const auto has_recursive_timed_mutex = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_mutex && finding.token == "std::recursive_timed_mutex";
    });
    const auto has_shared_mutex = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_mutex && finding.token == "std::shared_mutex";
    });
    const auto has_shared_timed_mutex = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_mutex && finding.token == "std::shared_timed_mutex";
    });
    const auto has_shared_lock = std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == lincheck::SourceAuditKind::standard_mutex && finding.token == "std::shared_lock";
    });

    require(has_atomic, "source audit should flag raw std::atomic");
    require(has_atomic_ref, "source audit should flag raw std::atomic_ref");
    require(has_condition, "source audit should flag raw std::condition_variable");
    require(has_jthread, "source audit should flag raw std::jthread");
    require(has_builtin, "source audit should flag compiler atomic builtins");
    require(has_sleep, "source audit should flag raw std::this_thread::sleep_for");
    require(has_barrier, "source audit should flag raw std::barrier");
    require(has_latch, "source audit should flag raw std::latch");
    require(has_semaphore, "source audit should flag raw std::binary_semaphore");
    require(has_recursive_mutex, "source audit should flag raw std::recursive_mutex");
    require(has_timed_mutex, "source audit should flag raw std::timed_mutex");
    require(has_recursive_timed_mutex, "source audit should flag raw std::recursive_timed_mutex");
    require(has_shared_mutex, "source audit should flag raw std::shared_mutex");
    require(has_shared_timed_mutex, "source audit should flag raw std::shared_timed_mutex");
    require(has_shared_lock, "source audit should flag raw std::shared_lock");

    lincheck::SourceAuditOptions token_filter;
    token_filter.allowed_token_substrings.push_back("__atomic_");
    const auto token_filtered = lincheck::audit_source_text("src/example.cpp", source, token_filter);
    require(token_filtered.size() == 15, "source audit should allow token filters");

    lincheck::SourceAuditOptions path_filter;
    path_filter.exclude_path_substrings.push_back("src/");
    require(
        lincheck::audit_source_text("src/example.cpp", source, path_filter).empty(),
        "source audit should allow path exclusion filters"
    );

    const auto report = lincheck::format_source_audit_report(findings);
    require(report.find("src/example.cpp:") != std::string::npos, "source audit report should include path and line");
    require(report.find("lincheck-source-audit") != std::string::npos, "source audit report should identify the tool");
}

void source_rewrite_instruments_standard_wrappers_and_filters_noise() {
    const std::string source = R"cpp(
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <latch>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <thread>

struct Example {
    std::atomic<int> value{0};
    int mirrored = 0;
    std::mutex lock;
    std::recursive_mutex recursive;
    std::timed_mutex timed;
    std::recursive_timed_mutex recursive_timed;
    std::shared_mutex shared;
    std::shared_timed_mutex shared_timed;
    std::condition_variable cv;
    std::barrier<> phase{2};
    std::binary_semaphore gate{0};
    std::counting_semaphore<3> permits{1};
    std::latch done{1};
    std::mutex suppressed; // NOLINT(lincheck-raw-sync)
    const char* text = "std::thread in a string literal";
    // std::jthread in a comment

    void run() {
        std::lock_guard<std::mutex> guard(lock);
        std::lock_guard<std::recursive_mutex> recursive_guard(recursive);
        std::unique_lock<std::timed_mutex> timed_guard(timed, std::defer_lock);
        std::shared_lock<std::shared_mutex> shared_guard(shared);
        std::shared_lock<std::shared_timed_mutex> shared_timed_guard(shared_timed, std::defer_lock);
        std::unique_lock<std::mutex> wait_lock(lock);
        std::thread worker([&] {
            std::this_thread::yield();
            std::atomic_ref<int> mirrored_ref(mirrored);
            mirrored_ref.fetch_add(1);
            value.fetch_add(1);
            gate.release();
            done.count_down();
            std::atomic_thread_fence(std::memory_order_seq_cst);
            phase.arrive_and_wait();
        });
        std::jthread service([&](std::stop_token token) {
            if (!token.stop_requested()) std::this_thread::yield();
        });
        gate.acquire();
        permits.acquire();
        permits.release();
        done.wait();
        phase.arrive_and_wait();
        if (timed.try_lock_for(std::chrono::milliseconds(0))) timed.unlock();
        if (timed_guard.try_lock_until(std::chrono::steady_clock::now())) timed_guard.unlock();
        if (recursive_timed.try_lock_for(std::chrono::milliseconds(0))) recursive_timed.unlock();
        if (shared.try_lock_shared()) shared.unlock_shared();
        if (shared_timed.try_lock_shared_for(std::chrono::milliseconds(0))) shared_timed.unlock_shared();
        if (shared_timed_guard.try_lock_for(std::chrono::milliseconds(0))) shared_timed_guard.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_until(std::chrono::steady_clock::now());
        worker.join();
    }
};
)cpp";

    const auto rewritten = lincheck::rewrite_source_text("src/example.cpp", source);
    require(rewritten.changed(), "source rewrite should report changes for raw standard wrappers");
    require(rewritten.added_lincheck_include, "source rewrite should add the Lincheck include");
    require(
        rewritten.text.rfind("#include <lincheck/lincheck.hpp>\n", 0) == 0,
        "source rewrite should prepend the Lincheck include"
    );
    require(
        rewritten.text.find("lincheck::atomic<int> value") != std::string::npos,
        "source rewrite should replace std::atomic"
    );
    require(
        rewritten.text.find("lincheck::atomic_ref<int> mirrored_ref(mirrored)") != std::string::npos,
        "source rewrite should replace std::atomic_ref"
    );
    require(
        rewritten.text.find("lincheck::mutex lock") != std::string::npos,
        "source rewrite should replace std::mutex"
    );
    require(
        rewritten.text.find("lincheck::recursive_mutex recursive") != std::string::npos,
        "source rewrite should replace std::recursive_mutex"
    );
    require(
        rewritten.text.find("lincheck::timed_mutex timed") != std::string::npos,
        "source rewrite should replace std::timed_mutex"
    );
    require(
        rewritten.text.find("lincheck::recursive_timed_mutex recursive_timed") != std::string::npos,
        "source rewrite should replace std::recursive_timed_mutex"
    );
    require(
        rewritten.text.find("lincheck::shared_mutex shared") != std::string::npos,
        "source rewrite should replace std::shared_mutex"
    );
    require(
        rewritten.text.find("lincheck::shared_timed_mutex shared_timed") != std::string::npos,
        "source rewrite should replace std::shared_timed_mutex"
    );
    require(
        rewritten.text.find("lincheck::condition_variable cv") != std::string::npos,
        "source rewrite should replace std::condition_variable"
    );
    require(
        rewritten.text.find("lincheck::barrier<> phase") != std::string::npos,
        "source rewrite should replace std::barrier"
    );
    require(
        rewritten.text.find("lincheck::binary_semaphore gate") != std::string::npos,
        "source rewrite should replace std::binary_semaphore"
    );
    require(
        rewritten.text.find("lincheck::counting_semaphore<3> permits") != std::string::npos,
        "source rewrite should replace std::counting_semaphore"
    );
    require(
        rewritten.text.find("lincheck::latch done") != std::string::npos,
        "source rewrite should replace std::latch"
    );
    require(
        rewritten.text.find("lincheck::lock_guard guard(lock)") != std::string::npos,
        "source rewrite should collapse std::lock_guard<std::mutex>"
    );
    require(
        rewritten.text.find("lincheck::lock_guard<lincheck::recursive_mutex> recursive_guard(recursive)") != std::string::npos,
        "source rewrite should support recursive_mutex lock guards"
    );
    require(
        rewritten.text.find("lincheck::unique_lock<lincheck::timed_mutex> timed_guard(timed, std::defer_lock)") !=
            std::string::npos,
        "source rewrite should support timed_mutex unique locks"
    );
    require(
        rewritten.text.find("lincheck::shared_lock<lincheck::shared_mutex> shared_guard(shared)") !=
            std::string::npos,
        "source rewrite should support shared_mutex shared locks"
    );
    require(
        rewritten.text.find("lincheck::shared_lock<lincheck::shared_timed_mutex> shared_timed_guard(shared_timed, std::defer_lock)") !=
            std::string::npos,
        "source rewrite should support shared_timed_mutex shared locks"
    );
    require(
        rewritten.text.find("lincheck::unique_lock wait_lock(lock)") != std::string::npos,
        "source rewrite should collapse std::unique_lock<std::mutex>"
    );
    require(
        rewritten.text.find("lincheck::thread worker") != std::string::npos,
        "source rewrite should replace std::thread"
    );
    require(
        rewritten.text.find("lincheck::jthread service") != std::string::npos,
        "source rewrite should replace std::jthread"
    );
    require(
        rewritten.text.find("lincheck::this_thread::yield()") != std::string::npos,
        "source rewrite should replace std::this_thread::yield"
    );
    require(
        rewritten.text.find("lincheck::this_thread::sleep_for(std::chrono::milliseconds(1))") != std::string::npos,
        "source rewrite should replace std::this_thread::sleep_for"
    );
    require(
        rewritten.text.find("lincheck::this_thread::sleep_until(std::chrono::steady_clock::now())") != std::string::npos,
        "source rewrite should replace std::this_thread::sleep_until"
    );
    require(
        rewritten.text.find("lincheck::atomic_thread_fence(std::memory_order_seq_cst)") != std::string::npos,
        "source rewrite should replace std::atomic_thread_fence"
    );
    require(
        rewritten.text.find("std::mutex suppressed") != std::string::npos,
        "source rewrite should honor line suppressions"
    );
    require(
        rewritten.text.find("\"std::thread in a string literal\"") != std::string::npos,
        "source rewrite should not rewrite string literals"
    );
    require(
        rewritten.text.find("// std::jthread in a comment") != std::string::npos,
        "source rewrite should not rewrite comments"
    );

    const auto findings_after_rewrite = lincheck::audit_source_text("src/example.cpp", rewritten.text);
    require(findings_after_rewrite.empty(), "rewritten source should pass the raw sync audit for supported replacements");

    lincheck::SourceRewriteOptions path_filter;
    path_filter.audit.exclude_path_substrings.push_back("src/");
    const auto skipped = lincheck::rewrite_source_text("src/example.cpp", source, path_filter);
    require(!skipped.changed(), "source rewrite should honor path exclusion filters");
    require(skipped.text == source, "source rewrite should leave excluded paths unchanged");

    const auto report = lincheck::format_source_rewrite_report(rewritten);
    require(report.find("lincheck-source-rewrite") != std::string::npos, "source rewrite report should identify the tool");
    require(report.find("std::thread") != std::string::npos, "source rewrite report should list replaced tokens");
}

void stm_hooks_emit_trace_events_and_switch_points() {
    RecordingRuntime runtime;
    int value = 0;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        lincheck::stm::tx_location_register(&value, "value", "int");
        lincheck::stm::tx_location_init(&value, value);
        lincheck::stm::tx_begin(false, 10);
        lincheck::stm::tx_attempt_metadata(1001, 1);
        lincheck::stm::tx_read(&value, 7, 10);
        lincheck::stm::tx_read_value(&value, 0, 7, 10);
        lincheck::stm::tx_write(&value, 7);
        lincheck::stm::tx_write_value(&value, 1, 7);
        lincheck::stm::tx_validate_begin();
        lincheck::stm::tx_validate_end(true);
        lincheck::stm::tx_lock_attempt(7);
        lincheck::stm::tx_lock_acquired(7);
        lincheck::stm::tx_lock_failed(7);
        lincheck::stm::tx_lock_released(7);
        lincheck::stm::tx_commit_attempt();
        lincheck::stm::tx_commit_success(11);
        lincheck::stm::tx_begin(false, 12);
        lincheck::stm::tx_abort("conflict");
        lincheck::stm::tx_retry("abort", 2);
        lincheck::stm::tx_begin(false, 0);
        lincheck::stm::tx_read(&value, 0, 0);
        lincheck::stm::tx_write(&value, 0);
        lincheck::stm::tx_lock_attempt(0);
        lincheck::stm::tx_lock_acquired(0);
        lincheck::stm::tx_lock_failed(0);
        lincheck::stm::tx_lock_released(0);
        lincheck::stm::tx_commit_success(0);
        lincheck::stm::tx_location_destroy(&value);
    }

    auto has_event = [&](const std::string& needle) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(needle) != std::string::npos;
        });
    };
    auto has_event_parts = [&](const std::string& first, const std::string& second) {
        return std::any_of(runtime.events.begin(), runtime.events.end(), [&](const std::string& event) {
            return event.find(first) != std::string::npos &&
                event.find(second) != std::string::npos;
        });
    };
    auto has_switch = [&](const std::string& needle) {
        return std::find(runtime.switches.begin(), runtime.switches.end(), needle) != runtime.switches.end();
    };

    const auto has_commit_switch = std::find(runtime.switches.begin(), runtime.switches.end(), "stm.tx_commit_success") != runtime.switches.end();
    const auto has_lock_release_switch = std::find(runtime.switches.begin(), runtime.switches.end(), "stm.tx_lock_released") != runtime.switches.end();
    const auto has_stable_address = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_read") != std::string::npos &&
            event.find("address=obj#") != std::string::npos &&
            event.find("address=0x") == std::string::npos;
    });

    require(has_event("stm.tx_begin"), "STM tx_begin hook should emit a trace event");
    require(has_event("stm.tx_location_register"), "STM location register hook should emit a trace event");
    require(has_event("stm.tx_location_init"), "STM location init hook should emit a trace event");
    require(has_event("stm.tx_location_destroy"), "STM location destroy hook should emit a trace event");
    require(has_event("stm.tx_attempt_metadata"), "STM attempt metadata hook should emit a trace event");
    require(has_event("stm.tx_read"), "STM tx_read hook should emit a trace event");
    require(has_event("stm.tx_write"), "STM tx_write hook should emit a trace event");
    require(has_event("stm.tx_validate_begin"), "STM tx_validate_begin hook should emit a trace event");
    require(has_event("stm.tx_validate_end success=true"), "STM tx_validate_end hook should emit a trace event");
    require(has_event("stm.tx_lock_attempt"), "STM tx_lock_attempt hook should emit a trace event");
    require(has_event("stm.tx_lock_acquired"), "STM tx_lock_acquired hook should emit a trace event");
    require(has_event("stm.tx_lock_failed"), "STM tx_lock_failed hook should emit a trace event");
    require(has_event("stm.tx_lock_released"), "STM tx_lock_released hook should emit a trace event");
    require(has_event("stm.tx_commit_attempt"), "STM tx_commit_attempt hook should emit a trace event");
    require(has_event("stm.tx_commit_success"), "STM tx_commit_success hook should emit a trace event");
    require(has_event("stm.tx_abort reason=conflict"), "STM tx_abort hook should emit a trace event");
    require(has_event("stm.tx_retry attempt=2 reason=abort"), "STM tx_retry hook should emit a trace event");
    require(has_event_parts("stm.tx_begin", "clock=0"), "STM begin hook should preserve a zero start clock");
    require(has_event_parts("stm.tx_location_init", "value=0"), "STM location init hook should preserve the initial value");
    require(has_event_parts("stm.tx_location_register", "label=value"), "STM location register hook should preserve labels");
    require(has_event_parts("stm.tx_location_register", "type=int"), "STM location register hook should preserve type names");
    require(has_event_parts("stm.tx_attempt_metadata", "logical_tx_id=1001"), "STM attempt metadata should preserve logical transaction IDs");
    require(has_event_parts("stm.tx_attempt_metadata", "attempt=1"), "STM attempt metadata should preserve attempts");
    require(has_event_parts("stm.tx_read", "lock_slot=0"), "STM read hook should preserve a zero lock slot");
    require(has_event_parts("stm.tx_read", "version=0"), "STM read hook should preserve a zero version");
    require(has_event_parts("stm.tx_read", "value=0"), "STM read value hook should preserve observed values");
    require(has_event_parts("stm.tx_write", "lock_slot=0"), "STM write hook should preserve a zero lock slot");
    require(has_event_parts("stm.tx_write", "value=1"), "STM write value hook should preserve written values");
    require(has_event("stm.tx_lock_attempt lock_slot=0"), "STM lock hook should preserve a zero lock slot");
    require(has_event_parts("stm.tx_commit_success", "clock=0"), "STM commit hook should preserve a zero commit clock");
    require(has_event_parts("stm.tx_begin", "tx_id=1"), "STM begin hook should assign a transaction ID");
    require(has_event_parts("stm.tx_read", "tx_id=1"), "STM read hook should preserve the active transaction ID");
    require(has_event_parts("stm.tx_commit_success", "tx_id=1"), "STM commit hook should preserve the active transaction ID");
    require(has_event_parts("stm.tx_abort", "tx_id=2"), "STM abort hook should preserve the aborted transaction ID");
    require(has_event_parts("stm.tx_retry", "tx_id=2"), "STM retry hook should preserve the aborted transaction ID");
    require(has_event_parts("stm.tx_begin", "tx_depth=1"), "STM begin hook should record transaction depth");
    require(has_event_parts("stm.tx_read", "logical_tx_id=1001"), "STM read value hook should preserve logical transaction IDs");
    require(has_event_parts("stm.tx_write", "logical_tx_id=1001"), "STM write value hook should preserve logical transaction IDs");
    require(has_stable_address, "STM read/write hooks should format addresses with stable object IDs");
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_begin &&
                event.has_clock &&
                event.clock == 0 &&
                !event.read_only;
        }),
        "raw STM begin events should preserve structured zero clock and read-only metadata"
    );
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_read &&
                event.has_lock_slot &&
                event.lock_slot == 0 &&
                event.has_version &&
                event.version == 0;
        }),
        "raw STM read events should preserve structured zero lock/version metadata"
    );
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_location_init &&
                event.has_value &&
                lincheck::value_cast<int>(event.value) == 0;
        }),
        "raw STM location init events should preserve structured values"
    );
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_read &&
                event.has_value &&
                lincheck::value_cast<int>(event.value) == 0 &&
                event.logical_transaction_id == 1001 &&
                event.attempt == 1;
        }),
        "raw STM read value events should preserve structured values and attempt metadata"
    );
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_write &&
                event.has_value &&
                lincheck::value_cast<int>(event.value) == 1 &&
                event.logical_transaction_id == 1001 &&
                event.attempt == 1;
        }),
        "raw STM write value events should preserve structured values and attempt metadata"
    );
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_validate_end && event.success;
        }),
        "raw STM validation events should preserve structured success metadata"
    );
    require(
        std::any_of(runtime.stm_events.begin(), runtime.stm_events.end(), [](const auto& event) {
            return event.kind == lincheck::stm::EventKind::tx_retry &&
                event.attempt == 2 &&
                event.reason == "abort";
        }),
        "raw STM retry events should preserve structured attempt and reason metadata"
    );
    require(has_switch("stm.tx_abort"), "STM abort hook should emit a scheduler switch point");
    require(has_switch("stm.tx_retry"), "STM retry hook should emit a scheduler switch point");
    require(has_switch("stm.tx_location_init"), "STM location init hook should emit a scheduler switch point");
    require(has_switch("stm.tx_attempt_metadata"), "STM attempt metadata hook should emit a scheduler switch point");
    require(has_commit_switch, "STM hooks should emit scheduler switch points");
    require(has_lock_release_switch, "STM lock release hook should emit a scheduler switch point");
}

void multiverse_hook_macros_bind_to_stm_hooks() {
    RecordingRuntime runtime;
    int value = 0;
    {
        lincheck::ScopedRuntime scoped(&runtime);
        MULTIVERSE_LINCHECK_TX_LOCATION_REGISTER(&value, "value", "int");
        MULTIVERSE_LINCHECK_TX_LOCATION_INIT(&value, value);
        MULTIVERSE_LINCHECK_TX_BEGIN(false, 20);
        MULTIVERSE_LINCHECK_TX_ATTEMPT_METADATA(2002, 4);
        MULTIVERSE_LINCHECK_TX_READ(&value, 3, 20);
        MULTIVERSE_LINCHECK_TX_READ_VALUE(&value, value, 3, 20);
        MULTIVERSE_LINCHECK_TX_LOCK_ATTEMPT(3);
        MULTIVERSE_LINCHECK_TX_LOCK_ACQUIRED(3);
        MULTIVERSE_LINCHECK_TX_LOCK_FAILED(3);
        MULTIVERSE_LINCHECK_TX_WRITE(&value, 3);
        MULTIVERSE_LINCHECK_TX_WRITE_VALUE(&value, 1, 3);
        MULTIVERSE_LINCHECK_TX_VALIDATE_BEGIN();
        MULTIVERSE_LINCHECK_TX_VALIDATE_END(false);
        MULTIVERSE_LINCHECK_TX_LOCK_RELEASED(3);
        MULTIVERSE_LINCHECK_TX_COMMIT_ATTEMPT();
        MULTIVERSE_LINCHECK_TX_COMMIT_SUCCESS(21);
        MULTIVERSE_LINCHECK_TX_ABORT("conflict");
        MULTIVERSE_LINCHECK_TX_RETRY("abort", 4);
        MULTIVERSE_LINCHECK_TX_LOCATION_DESTROY(&value);
    }

    const auto has_location_init = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_location_init") != std::string::npos &&
            event.find("value=0") != std::string::npos;
    });
    const auto has_location_register = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_location_register") != std::string::npos &&
            event.find("label=value") != std::string::npos &&
            event.find("type=int") != std::string::npos;
    });
    const auto has_location_destroy = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_location_destroy") != std::string::npos;
    });
    const auto has_attempt_metadata = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_attempt_metadata") != std::string::npos &&
            event.find("logical_tx_id=2002") != std::string::npos &&
            event.find("attempt=4") != std::string::npos;
    });
    const auto has_read_value = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_read") != std::string::npos &&
            event.find("value=0") != std::string::npos &&
            event.find("logical_tx_id=2002") != std::string::npos;
    });
    const auto has_write_value = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_write") != std::string::npos &&
            event.find("value=1") != std::string::npos &&
            event.find("logical_tx_id=2002") != std::string::npos;
    });
    const auto has_lock_attempt = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_lock_attempt") != std::string::npos;
    });
    const auto has_commit = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_commit_success") != std::string::npos;
    });
    const auto has_lock_release = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_lock_released") != std::string::npos;
    });
    const auto has_lock_failed = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_lock_failed") != std::string::npos;
    });
    const auto has_validation_failure = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_validate_end success=false") != std::string::npos;
    });
    const auto has_abort = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_abort reason=conflict") != std::string::npos;
    });
    const auto has_retry = std::any_of(runtime.events.begin(), runtime.events.end(), [](const std::string& event) {
        return event.find("stm.tx_retry attempt=4 reason=abort") != std::string::npos;
    });

    require(has_location_init, "Multiverse hook macros should bind location initialization to STM hooks");
    require(has_location_register, "Multiverse hook macros should bind location registration to STM hooks");
    require(has_location_destroy, "Multiverse hook macros should bind location destruction to STM hooks");
    require(has_attempt_metadata, "Multiverse hook macros should bind attempt metadata to STM hooks");
    require(has_read_value, "Multiverse hook macros should bind value reads to STM hooks");
    require(has_write_value, "Multiverse hook macros should bind value writes to STM hooks");
    require(has_lock_attempt, "Multiverse hook macros should bind lock attempts to STM hooks");
    require(has_lock_failed, "Multiverse hook macros should bind lock failures to STM hooks");
    require(has_validation_failure, "Multiverse hook macros should bind validation results to STM hooks");
    require(has_lock_release, "Multiverse hook macros should bind lock releases to STM hooks");
    require(has_commit, "Multiverse hook macros should bind commits to STM hooks");
    require(has_abort, "Multiverse hook macros should bind aborts to STM hooks");
    require(has_retry, "Multiverse hook macros should bind retries to STM hooks");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    const TestCase tests[] = {
        {"model_checker_finds_broken_counter", model_checker_finds_broken_counter},
        {"event_dependency_analysis_detects_invalid_edges_and_cycles", event_dependency_analysis_detects_invalid_edges_and_cycles},
        {"model_checker_replays_captured_schedule", model_checker_replays_captured_schedule},
        {"model_checker_replay_rejects_non_runnable_thread_choices", model_checker_replay_rejects_non_runnable_thread_choices},
        {"model_checker_checks_hand_authored_scenario", model_checker_checks_hand_authored_scenario},
        {"stress_runner_checks_hand_authored_scenario", stress_runner_checks_hand_authored_scenario},
        {"explicit_scenario_validation_rejects_bad_actors", explicit_scenario_validation_rejects_bad_actors},
        {"model_checker_uses_stm_hooks_as_switch_points", model_checker_uses_stm_hooks_as_switch_points},
        {"model_checker_context_switch_bound_limits_exploration", model_checker_context_switch_bound_limits_exploration},
        {"model_checker_records_observed_frontier_choices", model_checker_records_observed_frontier_choices},
        {"model_checker_seed_changes_schedule_exploration_order", model_checker_seed_changes_schedule_exploration_order},
        {"model_checker_seed_weights_same_depth_frontier_prefixes", model_checker_seed_weights_same_depth_frontier_prefixes},
        {"model_checker_reports_invocation_budget_pruning", model_checker_reports_invocation_budget_pruning},
        {"model_checker_prunes_duplicate_successful_public_histories", model_checker_prunes_duplicate_successful_public_histories},
        {"model_checker_can_prune_explicitly_independent_operation_contexts", model_checker_can_prune_explicitly_independent_operation_contexts},
        {"model_checker_can_prune_disjoint_operation_dependency_footprints", model_checker_can_prune_disjoint_operation_dependency_footprints},
        {"minimizer_removes_unneeded_init_and_post_actors", minimizer_removes_unneeded_init_and_post_actors},
        {"model_checker_finds_broken_var_counter", model_checker_finds_broken_var_counter},
        {"model_checker_finds_broken_var_operator_counter", model_checker_finds_broken_var_operator_counter},
        {"model_checker_finds_source_macro_counter", model_checker_finds_source_macro_counter},
        {"model_checker_accepts_atomic_counter", model_checker_accepts_atomic_counter},
        {"test_builder_supports_custom_object_factories", test_builder_supports_custom_object_factories},
        {"test_builder_supports_callable_operations", test_builder_supports_callable_operations},
        {"options_support_named_checks", options_support_named_checks},
        {"options_reject_invalid_counts", options_reject_invalid_counts},
        {"model_checker_reports_non_seq_cst_memory_order_warning_on_success", model_checker_reports_non_seq_cst_memory_order_warning_on_success},
        {"model_checker_accepts_custom_value_results", model_checker_accepts_custom_value_results},
        {"successful_checks_return_clock_warnings", successful_checks_return_clock_warnings},
        {"atomic_clock_source_is_global_across_instances", atomic_clock_source_is_global_across_instances},
        {"failure_trace_includes_clock_warnings", failure_trace_includes_clock_warnings},
        {"model_checker_trace_filters_runtime_lines_without_changing_execution", model_checker_trace_filters_runtime_lines_without_changing_execution},
        {"atomic_wrapper_supports_more_operations_and_memory_order_traces", atomic_wrapper_supports_more_operations_and_memory_order_traces},
        {"atomic_wrapper_rejects_invalid_memory_orders_before_std_atomic_calls", atomic_wrapper_rejects_invalid_memory_orders_before_std_atomic_calls},
        {"atomic_and_fence_wrappers_emit_structured_memory_events", atomic_and_fence_wrappers_emit_structured_memory_events},
        {"atomic_ref_wrapper_emits_structured_memory_events_for_referenced_object", atomic_ref_wrapper_emits_structured_memory_events_for_referenced_object},
        {"runtime_wrappers_emit_stable_object_ids", runtime_wrappers_emit_stable_object_ids},
        {"fence_helpers_emit_memory_order_and_location_traces", fence_helpers_emit_memory_order_and_location_traces},
        {"obstruction_freedom_accepts_operation_that_completes_in_isolation", obstruction_freedom_accepts_operation_that_completes_in_isolation},
        {"obstruction_freedom_accepts_expected_operation_exception_results", obstruction_freedom_accepts_expected_operation_exception_results},
        {"obstruction_freedom_rejects_operation_that_spins_in_isolation", obstruction_freedom_rejects_operation_that_spins_in_isolation},
        {"obstruction_freedom_rejects_operation_that_parks", obstruction_freedom_rejects_operation_that_parks},
        {"obstruction_freedom_rejects_operation_that_atomic_waits", obstruction_freedom_rejects_operation_that_atomic_waits},
        {"obstruction_freedom_rejects_operation_that_acquires_unavailable_semaphore", obstruction_freedom_rejects_operation_that_acquires_unavailable_semaphore},
        {"obstruction_freedom_rejects_operation_that_waits_on_closed_latch", obstruction_freedom_rejects_operation_that_waits_on_closed_latch},
        {"obstruction_freedom_rejects_operation_that_waits_on_incomplete_barrier", obstruction_freedom_rejects_operation_that_waits_on_incomplete_barrier},
        {"model_checker_reports_livelock_when_switch_budget_is_exceeded", model_checker_reports_livelock_when_switch_budget_is_exceeded},
        {"verifier_rejects_histories_that_violate_real_time_order", verifier_rejects_histories_that_violate_real_time_order},
        {"verifier_uses_optional_sequential_state_cache", verifier_uses_optional_sequential_state_cache},
        {"verifier_checks_hand_authored_stack_histories", verifier_checks_hand_authored_stack_histories},
        {"verifier_checks_hand_authored_queue_histories", verifier_checks_hand_authored_queue_histories},
        {"model_checker_finds_queue_like_capacity_bug", model_checker_finds_queue_like_capacity_bug},
        {"model_checker_accepts_optional_queue_results", model_checker_accepts_optional_queue_results},
        {"model_checker_accepts_expected_operation_exceptions", model_checker_accepts_expected_operation_exceptions},
        {"model_checker_rejects_mismatched_operation_exception_results", model_checker_rejects_mismatched_operation_exception_results},
        {"generator_respects_non_parallel_operation_groups", generator_respects_non_parallel_operation_groups},
        {"generator_preserves_plain_operation_group_metadata", generator_preserves_plain_operation_group_metadata},
        {"generator_respects_one_shot_operations", generator_respects_one_shot_operations},
        {"generator_resets_stateful_parameter_generators_per_scenario", generator_resets_stateful_parameter_generators_per_scenario},
        {"verifier_backtracks_parallel_orders_when_post_state_rejects_first_match", verifier_backtracks_parallel_orders_when_post_state_rejects_first_match},
        {"value_supports_stable_hashing", value_supports_stable_hashing},
        {"value_supports_custom_user_types", value_supports_custom_user_types},
        {"value_supports_std_optional_payloads", value_supports_std_optional_payloads},
        {"parameter_generators_support_plan_builtins_and_custom_seeded_values", parameter_generators_support_plan_builtins_and_custom_seeded_values},
        {"stress_runner_executes_correct_counter", stress_runner_executes_correct_counter},
        {"stress_runner_finds_forced_broken_counter_overlap", stress_runner_finds_forced_broken_counter_overlap},
        {"stress_runner_finds_forced_broken_queue_overlap", stress_runner_finds_forced_broken_queue_overlap},
        {"stress_runner_releases_parallel_workers_after_ready_gate", stress_runner_releases_parallel_workers_after_ready_gate},
        {"stress_runner_reports_non_seq_cst_memory_order_warning_on_success", stress_runner_reports_non_seq_cst_memory_order_warning_on_success},
        {"stress_runner_executes_locked_counter", stress_runner_executes_locked_counter},
        {"lock_guard_supports_raii_and_adopt_lock", lock_guard_supports_raii_and_adopt_lock},
        {"recursive_mutex_supports_reentrant_locking_and_raii", recursive_mutex_supports_reentrant_locking_and_raii},
        {"timed_mutex_supports_timed_locking_and_raii", timed_mutex_supports_timed_locking_and_raii},
        {"shared_mutex_supports_shared_and_exclusive_locking", shared_mutex_supports_shared_and_exclusive_locking},
        {"scoped_lock_supports_variadic_raii_and_adopt_lock", scoped_lock_supports_variadic_raii_and_adopt_lock},
        {"stress_runner_reports_verifier_explanation_for_invalid_results", stress_runner_reports_verifier_explanation_for_invalid_results},
        {"unique_lock_supports_standard_lock_tags", unique_lock_supports_standard_lock_tags},
        {"stress_runner_reports_cooperative_timeout", stress_runner_reports_cooperative_timeout},
        {"stress_runner_trace_filter_does_not_disable_timeout", stress_runner_trace_filter_does_not_disable_timeout},
        {"stress_runner_trace_filter_applies_without_timeout", stress_runner_trace_filter_applies_without_timeout},
        {"stress_runner_reports_validation_failure", stress_runner_reports_validation_failure},
        {"stress_runner_preserves_unexpected_exception_ptr", stress_runner_preserves_unexpected_exception_ptr},
        {"model_checker_reports_validation_failure", model_checker_reports_validation_failure},
        {"model_checker_preserves_unexpected_exception_ptr", model_checker_preserves_unexpected_exception_ptr},
        {"model_checker_aborts_peer_threads_after_primary_failure", model_checker_aborts_peer_threads_after_primary_failure},
        {"model_checker_deadlock_trace_includes_context_sections", model_checker_deadlock_trace_includes_context_sections},
        {"model_checker_accepts_locked_counter", model_checker_accepts_locked_counter},
        {"scheduler_reports_lock_blocking_and_unblocking", scheduler_reports_lock_blocking_and_unblocking},
        {"scheduler_reports_timed_lock_wake_and_timeout", scheduler_reports_timed_lock_wake_and_timeout},
        {"scheduler_reports_shared_lock_wake_and_timeout", scheduler_reports_shared_lock_wake_and_timeout},
        {"scheduler_abort_wakes_waiting_threads", scheduler_abort_wakes_waiting_threads},
        {"scheduler_basic_interleaving_matches_golden_trace", scheduler_basic_interleaving_matches_golden_trace},
        {"condition_variable_wait_notify_works_on_real_threads", condition_variable_wait_notify_works_on_real_threads},
        {"condition_variable_timed_wait_times_out_on_real_threads", condition_variable_timed_wait_times_out_on_real_threads},
        {"parker_park_unpark_works_on_real_threads", parker_park_unpark_works_on_real_threads},
        {"atomic_wait_notify_works_on_real_threads", atomic_wait_notify_works_on_real_threads},
        {"semaphore_acquire_release_works_on_real_threads", semaphore_acquire_release_works_on_real_threads},
        {"latch_wait_count_down_works_on_real_threads", latch_wait_count_down_works_on_real_threads},
        {"barrier_arrive_and_wait_works_on_real_threads", barrier_arrive_and_wait_works_on_real_threads},
        {"scheduler_reports_condition_variable_timed_wait_timeout", scheduler_reports_condition_variable_timed_wait_timeout},
        {"scheduler_reports_park_and_unpark", scheduler_reports_park_and_unpark},
        {"scheduler_reports_atomic_wait_and_notify", scheduler_reports_atomic_wait_and_notify},
        {"scheduler_reports_semaphore_acquire_and_release", scheduler_reports_semaphore_acquire_and_release},
        {"scheduler_reports_semaphore_timed_acquire_timeout", scheduler_reports_semaphore_timed_acquire_timeout},
        {"scheduler_reports_latch_wait_and_count_down", scheduler_reports_latch_wait_and_count_down},
        {"scheduler_reports_barrier_wait_and_phase_completion", scheduler_reports_barrier_wait_and_phase_completion},
        {"scheduler_reports_condition_variable_wait_and_notify", scheduler_reports_condition_variable_wait_and_notify},
        {"thread_wrapper_propagates_runtime_callbacks", thread_wrapper_propagates_runtime_callbacks},
        {"thread_wrapper_supports_arguments_joinable_id_and_detach", thread_wrapper_supports_arguments_joinable_id_and_detach},
        {"thread_wrapper_rethrows_child_exception_on_join", thread_wrapper_rethrows_child_exception_on_join},
        {"jthread_wrapper_supports_stop_token_runtime_and_auto_join", jthread_wrapper_supports_stop_token_runtime_and_auto_join},
        {"jthread_wrapper_rethrows_child_exception_on_join", jthread_wrapper_rethrows_child_exception_on_join},
        {"this_thread_sleep_wrappers_emit_trace_and_switch_points", this_thread_sleep_wrappers_emit_trace_and_switch_points},
        {"run_concurrent_test_records_threaded_block_trace", run_concurrent_test_records_threaded_block_trace},
        {"run_concurrent_test_reports_non_seq_cst_memory_order_warning", run_concurrent_test_reports_non_seq_cst_memory_order_warning},
        {"run_concurrent_test_retains_source_metadata_for_fence_events", run_concurrent_test_retains_source_metadata_for_fence_events},
        {"run_concurrent_test_records_park_unpark_trace", run_concurrent_test_records_park_unpark_trace},
        {"run_concurrent_test_applies_trace_filter", run_concurrent_test_applies_trace_filter},
        {"run_concurrent_test_preserves_child_exception", run_concurrent_test_preserves_child_exception},
        {"source_macros_emit_location_metadata", source_macros_emit_location_metadata},
        {"source_audit_detects_raw_sync_and_filters_noise", source_audit_detects_raw_sync_and_filters_noise},
        {"source_rewrite_instruments_standard_wrappers_and_filters_noise", source_rewrite_instruments_standard_wrappers_and_filters_noise},
        {"stm_hooks_emit_trace_events_and_switch_points", stm_hooks_emit_trace_events_and_switch_points},
        {"multiverse_hook_macros_bind_to_stm_hooks", multiverse_hook_macros_bind_to_stm_hooks},
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
