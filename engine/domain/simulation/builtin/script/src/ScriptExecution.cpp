#include <lux/engine/simulation/script/ScriptExecution.hpp>

namespace lux::simulation::script::detail
{
    void ScriptExecution::prepare(ScriptRuntimeLimits configured, std::size_t instance_capacity,
        std::size_t method_capacity, ScriptExecutionFailureSink failures)
    {
        limits_ = configured;
        failures_ = failures;
        execution_instances_.resize(instance_capacity);
        active_hooks_.resize(method_capacity);
        cells_.prepare(limits_.continuation_capacity, limits_.awaitable_capacity);
        continuations_.reserve(limits_.continuation_capacity);
        awaitables_.reserve(limits_.awaitable_capacity);
        resumes_.prepare(limits_.resume_queue_capacity);
    }

    void ScriptExecution::shutdown() noexcept
    {
        if (!continuations_.empty() || !awaitables_.empty() || result_write_pins_ != 0U)
            std::terminate();
        execution_instances_.clear();
        active_hooks_.clear();
        continuations_.clear();
        awaitables_.clear();
        resumes_.clear();
        cells_.clear();
    }

    void ScriptExecution::writeStats(ScriptRuntimeStats& result) const noexcept
    {
        result.sync_invocations = sync_invocations_;
        result.step_invocations = step_invocations_;
        result.backend_resume_calls = backend_resume_calls_;
        result.suspensions_admitted = suspensions_admitted_;
        result.active_continuations = continuations_.size();
        result.active_awaitables = activeAwaitables();
        result.instance_cleanup_awaitable_visits = instance_cleanup_awaitable_visits_;
        result.instance_cleanup_continuation_visits = instance_cleanup_continuation_visits_;
        result.event_payload_copy_bytes = event_payload_copy_bytes_;
        result.result_write_pins = result_write_pins_;
        result.deferred_awaitable_releases = pending_awaitable_releases_;
        result.awaitable_record_bytes = sizeof(ScriptLocalWait);
        result.awaitable_reserved_slots = awaitables_.capacity();
        result.awaitable_storage_bytes = awaitables_.storageBytes();
        result.operation_cell_bytes = sizeof(ScriptOperationCell);
        result.operation_cell_capacity = cells_.capacity();
        result.operation_cells_active = cells_.used();
        result.operation_cell_backing_bytes = cells_.bankBytes();
        result.continuation_directory_bytes = cells_.continuationDirectoryBytes();
        result.boxed_wait_body_bytes = sizeof(ScriptBoxedWait);
        result.resume_record_bytes = sizeof(ResumeRecord);
        result.resume_backing_bytes = resumes_.records.capacity() * sizeof(ResumeRecord);
#if defined(LUX_SCRIPT_HOTPATH_OBSERVATION)
        result.cell_observation_enabled = true;
        const auto& observation = cells_.observation();
        result.cell_cell_acquires = observation.cell_acquires;
        result.cell_cell_releases = observation.cell_releases;
        result.cell_in_place_promotions = observation.in_place_promotions;
        result.cell_execution_body_creations = observation.execution_body_creations;
        result.cell_local_waits = observation.local_waits;
        result.cell_local_rearms = observation.local_rearms;
        result.cell_boxed_external = observation.boxed_external;
        result.cell_boxed_no_scope = observation.boxed_no_scope;
        result.cell_boxed_layout = observation.boxed_layout;
        result.cell_boxed_occupied_local = observation.boxed_occupied_local;
        result.cell_wait_admissions = observation.wait_admissions;
        result.cell_wait_releases = observation.wait_releases;
        result.cell_execution_admissions = observation.execution_admissions;
        result.cell_execution_releases = observation.execution_releases;
        result.cell_source_direct_hits = source_direct_hits_;
        result.cell_source_directory_lookups = source_directory_lookups_;
        result.cell_stale_pops = stale_pops_;
        result.cell_pin_deferrals = pin_deferrals_;
#endif
        result.resume_queue_depth = resumes_.count;
        result.resume_queue_high_water = resumes_.high_water;
    }
}
