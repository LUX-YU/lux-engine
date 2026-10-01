#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
namespace lux::editor::material
{
    persistence::PersistenceResult<persistence::WriteTicket> publishCompiledMaterial(
        persistence::WriteCoordinator& writes,
        persistence::WriteTarget target,
        std::shared_ptr<const CompiledMaterial> compiled
    )
    {
        using namespace persistence;
        if (!compiled || !compiled->artifact || !compiled->source || compiled->bytes.empty())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        return publishEncodedArtifact(writes, std::move(target), EncodedArtifact{compiled->bytes});
    }
}
