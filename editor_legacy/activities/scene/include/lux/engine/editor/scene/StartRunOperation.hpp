#pragma once

#include <lux/engine/editor/scene/RunTypes.hpp>
#include <memory>

namespace lux::editor::scene
{
    class RunStore;
    // Unique public control owner; the worker retains only the existing completion state.
    class StartRunOperation final
    {
    public:
        ~StartRunOperation();
        StartRunOperation(const StartRunOperation&) = delete;
        StartRunOperation& operator=(const StartRunOperation&) = delete;
        StartRunOperation(StartRunOperation&&) = delete;
        StartRunOperation& operator=(StartRunOperation&&) = delete;
        void cancel() noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] StartRunId id() const noexcept;

    private:
        friend class RunStore;
        struct Impl;
        explicit StartRunOperation(std::shared_ptr<Impl>);
        std::shared_ptr<Impl> impl_;
    };
}
