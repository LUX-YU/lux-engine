#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/flowforge/FlowCodec.hpp>
#include <lux/engine/editor/flowforge/FlowEnvironment.hpp>

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::editor::flowforge
{
    extern const services::ServiceDescriptor kFlowEnvironmentService;
    // Owner-stage capture of the registered reflection set; no compiler, task or UI is created.
    [[nodiscard]] lux::flowforge::FlowSourceResult<FlowEnvironment> captureFlowEnvironment();

    [[nodiscard]] sessions::SessionPreparation prepareFlowSession(
        PreparedFlowData data,
        [[nodiscard]] sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        lux::flowforge::FlowSourceEnvironment environment,
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeFlowSessionFactory(
        lux::flowforge::FlowSourceEnvironment environment,
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeNewFlowCommand(
        commands::CommandEntry::Query,
        sessions::SessionCreation,
        lux::flowforge::FlowSourceEnvironment
    );
} // namespace lux::editor::flowforge
