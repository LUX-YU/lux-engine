#pragma once

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include <lux/engine/simulation/script/ScriptWaitSource.hpp>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::simulation::script::detail
{
    class ScriptExecution;

    class ScriptTimers final
    {
    public:
        using StartResult = lux::script::ScriptAbilityStartResult;
        using Completion = lux::script::ScriptAbilityCompletion<void>;
        void prepare(const SimulationClock& clock, ScriptRuntimeLimits limits, ScriptRealDelayEndpoint real_delay,
            ScriptExecution& execution, std::size_t instance_capacity);
        void beginInstance(ScriptInstanceId instance) noexcept;
        [[nodiscard]] StartResult nextStep(Completion completion) noexcept;
        [[nodiscard]] StartResult seconds(double duration, Completion completion) noexcept;
        [[nodiscard]] StartResult simulationSeconds(double duration, Completion completion) noexcept;
        [[nodiscard]] StartResult realSeconds(double duration, Completion completion) noexcept;
        [[nodiscard]] bool promoteNextStep() noexcept;
        [[nodiscard]] bool promoteSimulationDelay() noexcept;
        [[nodiscard]] std::optional<ScriptSourceCancellation> cancel(ScriptSourceId id) noexcept;
        [[nodiscard]] std::optional<ScriptSourceCancellation> cancelNext(ScriptInstanceId instance) noexcept;
        void stop() noexcept { stopping_ = true; }
        void shutdown() noexcept;
        void writeStats(ScriptRuntimeStats& stats) const noexcept;

    private:
        enum class ETimerKind : std::uint8_t { NEXT_STEP, SIMULATION_DELAY };
        struct TimerTag;
        struct Wait final
        {
            ScriptSourceId id;
            ScriptTimerAssociation association;
            Completion completion;
            ETimerKind kind{};
            SimulationDuration deadline{};
            std::uint64_t minimum_step{};
            std::uint64_t sequence{};
            std::size_t heap_index{};
            ScriptSourceId previous;
            ScriptSourceId next;
            ScriptSourceId instance_previous;
            ScriptSourceId instance_next;
        };
        struct InstanceIndex final
        {
            ScriptInstanceId id;
            ScriptSourceId first;
        };
        using Storage = lux::cxx::SlotMap<Wait, TimerTag>;
        using Key = Storage::key_type;
        [[nodiscard]] static Key key(ScriptSourceId id) noexcept
        {
            return id.valid() ? Key{id.slot - 1U, id.generation} : Key::invalid();
        }
        [[nodiscard]] static StartResult error(EScriptDelayStatus status) noexcept;
        [[nodiscard]] StartResult registerWait(ETimerKind kind, ScriptTimerAssociation association,
            Completion completion, SimulationDuration deadline, std::uint64_t step) noexcept;
        [[nodiscard]] bool earlier(ScriptSourceId left, ScriptSourceId right) const noexcept;
        void swapHeap(std::size_t left, std::size_t right) noexcept;
        void siftUp(std::size_t index) noexcept;
        void siftDown(std::size_t index) noexcept;
        void eraseHeap(std::size_t index) noexcept;
        [[nodiscard]] bool completeDue(ScriptSourceId id, bool& backpressured) noexcept;

        const SimulationClock* clock_{};
        ScriptExecution* execution_{};
        ScriptRealDelayEndpoint real_delay_;
        Storage waits_;
        std::vector<ScriptSourceId> heap_;
        std::vector<InstanceIndex> instances_;
        ScriptSourceId next_first_;
        ScriptSourceId next_last_;
        std::size_t next_capacity_{};
        std::size_t delay_capacity_{};
        std::size_t next_count_{};
        std::uint64_t sequence_{};
        bool stopping_{};
    };
}
