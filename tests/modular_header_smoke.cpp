#include <lincheck/clock.hpp>
#include <lincheck/generators.hpp>
#include <lincheck/model_checking.hpp>
#include <lincheck/options.hpp>
#include <lincheck/runtime.hpp>
#include <lincheck/scenario.hpp>
#include <lincheck/source_audit.hpp>
#include <lincheck/stm.hpp>
#include <lincheck/stress.hpp>
#include <lincheck/trace.hpp>
#include <lincheck/value.hpp>
#include <lincheck/verifier.hpp>
#include <lincheck/wrappers/atomic.hpp>
#include <lincheck/wrappers/barrier.hpp>
#include <lincheck/wrappers/condition_variable.hpp>
#include <lincheck/wrappers/latch.hpp>
#include <lincheck/wrappers/mutex.hpp>
#include <lincheck/wrappers/parker.hpp>
#include <lincheck/wrappers/recursive_mutex.hpp>
#include <lincheck/wrappers/recursive_timed_mutex.hpp>
#include <lincheck/wrappers/semaphore.hpp>
#include <lincheck/wrappers/shared_lock.hpp>
#include <lincheck/wrappers/shared_mutex.hpp>
#include <lincheck/wrappers/shared_timed_mutex.hpp>
#include <lincheck/wrappers/thread.hpp>
#include <lincheck/wrappers/timed_mutex.hpp>
#include <lincheck/wrappers/var.hpp>

#include <chrono>
#include <string>

int main() {
    lincheck::Value value(1);
    auto generator = lincheck::gen<int>();
    lincheck::AtomicClockSource clock;
    lincheck::MemoryModel memory_model = lincheck::MemoryModel::sequential_consistency;
    lincheck::TraceEventRecord trace_event_record;
    lincheck::MemoryEvent memory_event;
    lincheck::MemoryEventRecord memory_event_record;
    lincheck::StmEventRecord stm_event_record;
    lincheck::SourceAccessEvent source_access_event;
    lincheck::SourceAccessEventRecord source_access_record;
    lincheck::SynchronizationEvent synchronization_event;
    lincheck::SynchronizationEventRecord synchronization_record;
    lincheck::OperationContext operation_context;
    lincheck::EventDependencyNode dependency_node;
    lincheck::EventDependencyEdge dependency_edge;
    lincheck::EventDependencyGraph dependency_graph;
    lincheck::EventDependencyAnalysis dependency_analysis;
    lincheck::OperationDependencyFootprint dependency_footprint;
    lincheck::CheckStats stats;
    lincheck::TraceFilter filter;
    lincheck::ExecutionScenario scenario;
    lincheck::SourceAuditOptions audit_options;
    lincheck::SourceRewriteOptions rewrite_options;
    lincheck::SourceRewriteEdit rewrite_edit;
    lincheck::SourceRewriteResult rewrite_result;
    lincheck::StressOptions stress;
    lincheck::ModelCheckingOptions model;
    lincheck::atomic<int> atomic_value{0};
    lincheck::var<int> plain_value{0};
    lincheck::mutex mutex;
    {
        lincheck::scoped_lock scoped(mutex);
    }
    lincheck::condition_variable condition;
    lincheck::recursive_mutex recursive_mutex;
    lincheck::timed_mutex timed_mutex;
    lincheck::recursive_timed_mutex recursive_timed_mutex;
    lincheck::shared_mutex shared_mutex;
    lincheck::shared_timed_mutex shared_timed_mutex;
    {
        lincheck::lock_guard recursive_guard(recursive_mutex);
    }
    {
        lincheck::unique_lock timed_lock(timed_mutex, std::defer_lock);
        (void)timed_lock.try_lock_for(std::chrono::milliseconds(0));
    }
    if (timed_mutex.try_lock_until(std::chrono::steady_clock::now())) {
        timed_mutex.unlock();
    }
    if (recursive_timed_mutex.try_lock_for(std::chrono::milliseconds(0))) {
        recursive_timed_mutex.unlock();
    }
    {
        lincheck::shared_lock shared_guard(shared_mutex);
    }
    if (shared_mutex.try_lock_shared()) {
        shared_mutex.unlock_shared();
    }
    {
        lincheck::shared_lock timed_shared_lock(shared_timed_mutex, std::defer_lock);
        (void)timed_shared_lock.try_lock_for(std::chrono::milliseconds(0));
    }
    if (shared_timed_mutex.try_lock_shared_until(std::chrono::steady_clock::now())) {
        shared_timed_mutex.unlock_shared();
    }
    lincheck::parker parker;
    lincheck::binary_semaphore semaphore{1};
    lincheck::latch latch{0};
    lincheck::barrier<> barrier{1};

    (void)value;
    (void)generator;
    (void)clock;
    (void)memory_model;
    trace_event_record.kind = "smoke";
    trace_event_record.description = "smoke event";
    (void)lincheck::to_string(trace_event_record);
    (void)trace_event_record;
    (void)memory_event;
    memory_event.has_source = true;
    memory_event.source = lincheck::source_location(__FILE__, __LINE__, __func__);
    memory_event_record.object_id = "obj#1";
    memory_event_record.has_source = true;
    memory_event_record.location_id = "loc#1";
    (void)memory_event_record;
    stm_event_record.kind = "tx_begin";
    stm_event_record.description = "stm.tx_begin";
    stm_event_record.has_clock = true;
    stm_event_record.reason = "smoke";
    (void)stm_event_record;
    source_access_event.kind = lincheck::SourceAccessKind::read;
    source_access_record.location_id = "loc#1";
    (void)source_access_event;
    (void)source_access_record;
    synchronization_event.kind = lincheck::SynchronizationEventKind::mutex_lock_attempt;
    synchronization_event.has_success = true;
    synchronization_event.success = true;
    synchronization_record.object_id = "obj#1";
    (void)synchronization_event;
    (void)synchronization_record;
    operation_context.thread_id = 0;
    operation_context.actor_index = 0;
    operation_context.name = "op";
    operation_context.independence_group = "readonly";
    (void)operation_context;
    dependency_node.stream = "memory";
    dependency_edge.kind = lincheck::EventDependencyEdgeKind::stream_resource_order;
    dependency_edge.resource_id = "obj#1";
    dependency_graph.nodes.push_back(dependency_node);
    lincheck::ScheduleDecision decision;
    decision.runnable_operations.push_back(operation_context);
    (void)decision;
    dependency_graph.edges.push_back(dependency_edge);
    dependency_analysis = lincheck::analyze_event_dependency_graph(dependency_graph);
    dependency_footprint.operation = operation_context;
    dependency_footprint.resources.push_back("obj#1");
    stats.schedules_pruned_by_event_dependency = 1;
    (void)lincheck::format_event_dependency_graph_json(dependency_graph, &dependency_analysis);
    (void)lincheck::format_event_dependency_graph_dot(dependency_graph);
    (void)lincheck::build_operation_dependency_footprints(dependency_graph);
    (void)dependency_graph;
    (void)dependency_analysis;
    (void)dependency_footprint;
    (void)stats;
    (void)lincheck::memory_event_kind_name(lincheck::MemoryEventKind::atomic_load);
    (void)lincheck::source_access_kind_name(lincheck::SourceAccessKind::write);
    (void)lincheck::synchronization_event_kind_name(lincheck::SynchronizationEventKind::mutex_lock_attempt);
    (void)lincheck::event_dependency_edge_kind_name(lincheck::EventDependencyEdgeKind::stream_resource_order);
    (void)filter;
    (void)scenario;
    (void)audit_options;
    rewrite_edit.token = "std::mutex";
    rewrite_result.text = "lincheck::mutex";
    (void)rewrite_options;
    (void)rewrite_edit;
    (void)rewrite_result;
    (void)lincheck::independent_operation_group("readonly");
    model.operation_context_reduction(false);
    model.event_dependency_reduction(false);
    (void)lincheck::rewrite_source_text("smoke.cpp", "std::mutex value;");
    (void)stress;
    (void)model;
    (void)atomic_value;
    (void)plain_value;
    (void)mutex;
    (void)condition;
    (void)recursive_mutex;
    (void)timed_mutex;
    (void)recursive_timed_mutex;
    (void)shared_mutex;
    (void)shared_timed_mutex;
    (void)parker;
    (void)semaphore;
    (void)latch;
    (void)barrier;
    (void)lincheck::stm::event_name(lincheck::stm::EventKind::tx_begin);

    return 0;
}
