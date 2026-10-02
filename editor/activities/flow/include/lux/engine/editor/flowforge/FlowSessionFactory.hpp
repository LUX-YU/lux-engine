#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowCodec.hpp>

namespace lux::editor::flowforge
{
    [[nodiscard]] sessions::SessionPreparation prepareFlowSession(
        PreparedFlowData data,
        [[nodiscard]] sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        lux::flowforge::FlowSourceEnvironment environment,
        contracts::CodeLease code = contracts::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeFlowSessionFactory(
        lux::flowforge::FlowSourceEnvironment environment,
        contracts::CodeLease code = contracts::CodeLease::builtin()
    );
}
