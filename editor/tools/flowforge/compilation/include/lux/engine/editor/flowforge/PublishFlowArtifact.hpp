#pragma once
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
namespace lux::editor::flowforge
{
    [[nodiscard]] persistence::PersistenceResult<persistence::WriteTicket>
    publishFlowArtifact(persistence::WriteCoordinator&, persistence::WriteTarget, std::shared_ptr<const CompiledFlow>);
}
