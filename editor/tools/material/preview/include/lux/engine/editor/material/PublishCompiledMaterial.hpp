#pragma once
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/persistence/WriteCoordinator.hpp>
namespace lux::editor::material
{
    // Only admits an immutable derived artifact to the shared lane. The coordinator owns
    // publication and its receipt; this operation has no author/session adoption capability.
    class PublishCompiledMaterialOperation final
    {
    public:
        [[nodiscard]] static persistence::PersistenceResult<persistence::WriteTicket>
        start(persistence::WriteCoordinator&, persistence::WriteTarget, std::shared_ptr<const CompiledMaterial>);
    };
}
