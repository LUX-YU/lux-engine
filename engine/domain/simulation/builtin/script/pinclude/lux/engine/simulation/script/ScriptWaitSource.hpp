#pragma once

#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

namespace lux::simulation::script::detail
{
    struct ScriptOperationCell;

    struct ScriptCellTicket final
    {
        ScriptOperationCell* cell{};
        std::uint64_t epoch{};
        [[nodiscard]] explicit operator bool() const noexcept { return cell != nullptr; }
        friend bool operator==(ScriptCellTicket, ScriptCellTicket) noexcept = default;
    };

    // Owner-thread capability. The stable header is checked before entering the body union.
    struct LocalWaitTicket final
    {
        ScriptCellTicket cell;
        std::uint64_t wait_epoch{};
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(cell); }
        friend bool operator==(LocalWaitTicket, LocalWaitTicket) noexcept = default;
    };

    struct ScriptSourceId final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};
        [[nodiscard]] constexpr bool valid() const noexcept { return slot != 0U && generation != 0U; }
        friend constexpr bool operator==(ScriptSourceId, ScriptSourceId) noexcept = default;
    };

    enum class EScriptWaitSource : std::uint8_t
    {
        NONE,
        EVENT,
        TIMER,
    };

    struct ScriptWaitSource final
    {
        ScriptSourceId id;
        EScriptWaitSource kind{EScriptWaitSource::NONE};
        friend constexpr bool operator==(ScriptWaitSource, ScriptWaitSource) noexcept = default;
    };

    struct ScriptSourceCancellation final
    {
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
        ScriptWaitSource source;
    };

    struct ScriptTimerAssociation final
    {
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
        LocalWaitTicket local;
    };

    // Only for the synchronous no-user-code interval between source admission and commit.
    // Timers retain the identity pair, never this transient borrow of Execution's result storage.
    class ScriptTimerAdmission final
    {
    public:
        [[nodiscard]] ScriptTimerAssociation association() const noexcept { return association_; }
    private:
        friend class ScriptExecution;
        ScriptTimerAdmission(ScriptTimerAssociation association, void* result) noexcept
            : association_(association), result_(result) {}
        ScriptTimerAssociation association_;
        void* result_{};
    };
}
