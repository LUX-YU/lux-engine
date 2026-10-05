#pragma once
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <optional>
#include <thread>

namespace fixture
{
    struct Trace final
    {
        std::thread::id owner{std::this_thread::get_id()};
        unsigned created{}, destroyed{}, destructor_returns{}, windows{}, windows_destroyed{};
        unsigned received{}, shown{}, notifications{}, unloaded{}, operations{};
        lux::process::TaskId submitted;
    };
    class Job : public lux::object::LuxObject
    {
    public:
        using LuxObject::LuxObject;
        lux::object::TSignal<int> changed{*this};
        using StartResult = lux::cxx::expected<lux::process::TaskId, lux::process::EExecutionError>;
        virtual StartResult start(std::shared_ptr<Job>) noexcept = 0;
        virtual std::optional<int> result() const noexcept = 0;
        virtual void acknowledge() noexcept = 0;
    };
    using ReadDefinitions =
        void (*)(lux::object::CodeLease, std::shared_ptr<const lux::services::ServiceEntry>&, std::vector<std::shared_ptr<const lux::editor::desktop::UiEntry>>&);
} // namespace fixture
