#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
namespace lux::editor::flowforge
{
    persistence::PersistenceResult<persistence::WriteTicket> publishFlowArtifact(
        persistence::WriteCoordinator& writes,
        persistence::WriteTarget target,
        std::shared_ptr<const CompiledFlow> compiled
    )
    {
        using namespace persistence;
        if (!compiled || !compiled->artifact || !compiled->source || compiled->bytes.empty())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        return publishEncodedArtifact(writes, std::move(target), EncodedArtifact{compiled->bytes});
    }
}
