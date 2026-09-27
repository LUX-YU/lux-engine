#pragma once

#include <lux/cxx/container/SlotMap.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lux::process
{
    struct TaskTag;

    struct TaskId final
    {
        std::uint64_t runtime{};
        lux::cxx::SlotKey<TaskTag> slot;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return runtime != 0U && slot.isValid();
        }
        friend bool operator==(TaskId, TaskId) noexcept = default;
    };

    enum class ETaskState : std::uint8_t
    {
        QUEUED,
        RUNNING,
        SUCCEEDED,
        FAILED,
        CANCELLED
    };

    struct TaskOptions final
    {
        std::string name;
        std::string category;
        TaskId parent;
        std::shared_ptr<const void> code_lifetime;
    };

    struct TaskProgress final
    {
        std::uint64_t completed{};
        std::uint64_t total{};
    };

    struct TaskInfo final
    {
        TaskId id;
        TaskId parent;
        std::string name;
        std::string category;
        std::string phase;
        ETaskState state{ETaskState::QUEUED};
        std::optional<TaskProgress> progress;
        std::chrono::steady_clock::time_point submitted;
        std::optional<std::chrono::steady_clock::time_point> started;
        std::optional<std::chrono::steady_clock::time_point> finished;
        bool cancel_requested{};
    };
}
