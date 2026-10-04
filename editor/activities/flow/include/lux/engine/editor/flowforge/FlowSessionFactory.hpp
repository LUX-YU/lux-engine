#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/flowforge/FlowCodec.hpp>

namespace lux::editor::flowforge
{
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
