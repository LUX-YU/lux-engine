#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
namespace lux::editor::material
{
    persistence::PersistenceResult<persistence::WriteTicket> PublishCompiledMaterialOperation::start(
        persistence::WriteCoordinator& writes,
        persistence::WriteTarget target,
        std::shared_ptr<const CompiledMaterial> compiled
    )
    {
        using namespace persistence;
        if (!compiled || !compiled->artifact || !compiled->source || compiled->bytes.empty())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        // No source-session origin: a compiled format must not inherit a source-save version chain.
        // The caller explicitly resolves the current target version, even for the same author.
        auto ticket = writes.reserve(std::move(target), {});
        if (!ticket)
            return lux::cxx::unexpected(ticket.error());
        auto admitted = writes.provideEncoded(
            *ticket,
            EncodedArtifact{{compiled->bytes.data(), compiled->bytes.data() + compiled->bytes.size()}}
        );
        if (!admitted)
        {
            const auto cancelled = writes.cancelBeforePublish(*ticket, admitted.error());
            if (cancelled)
                static_cast<void>(writes.acknowledge(*ticket));
            return lux::cxx::unexpected(admitted.error());
        }
        return *ticket;
    }
}
