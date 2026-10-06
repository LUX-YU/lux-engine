#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
namespace lux::editor::material
{
    [[nodiscard]] persistence::PersistenceResult<persistence::DerivedArtifact>
    captureMaterialArtifact(std::shared_ptr<const CompiledMaterial>);

    [[nodiscard]] persistence::PersistenceResult<persistence::WriteTicket>
    publishCompiledMaterial(
        persistence::WriteCoordinator&, persistence::WriteTarget, std::shared_ptr<const CompiledMaterial>
    );
}
