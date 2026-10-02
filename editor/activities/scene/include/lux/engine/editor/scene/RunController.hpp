#pragma once

#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>

namespace lux::editor::scene
{
    class StartRunOperation final
    {
    public:
        ~StartRunOperation();
        StartRunOperation(const StartRunOperation&) = delete;
        StartRunOperation& operator=(const StartRunOperation&) = delete;
        void cancel() noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] StartRunId id() const noexcept;

    private:
        friend class RunController;
        struct Impl;
        explicit StartRunOperation(std::shared_ptr<Impl>);
        std::shared_ptr<Impl> impl_;
    };

    class RunController final
    {
    public:
        explicit RunController(RunStore& store) noexcept : store_(store) {}
        [[nodiscard]] RunResult<std::unique_ptr<StartRunOperation>> prepare(
            SceneSession&,
            RunEnvironment,
            RunConfiguration = {}
        );
        [[nodiscard]] RunResult<std::unique_ptr<StartRunOperation>> prepare(
            SceneSnapshot,
            RunEnvironment,
            RunConfiguration = {}
        );
        // Owner safe point only. A RunId is published only after successful instance construction.
        [[nodiscard]] RunResult<RunId> adopt(StartRunOperation&);

    private:
        [[nodiscard]] RunResult<std::unique_ptr<StartRunOperation>> launch(std::shared_ptr<StartRunOperation::Impl>);
        RunStore& store_;
    };
}
