#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
namespace lux::editor::flowforge
{
    namespace
    {
        class FlowArtifactSource final : public persistence::IArtifactSource
        {
        public:
            explicit FlowArtifactSource(std::shared_ptr<const CompiledFlow> compiled)
                : compiled_(std::move(compiled))
            {}
            persistence::PersistenceResult<persistence::EncodedArtifact> encode(std::stop_token stop) const override
            {
                if (stop.stop_requested())
                    return cxx::unexpected(persistence::PersistenceFailure{persistence::EPersistenceError::CANCELLED});
                auto encoded = lux::flowforge::encodeFlowSource(*compiled_->source);
                if (!encoded)
                    return cxx::unexpected(persistence::PersistenceFailure{
                        persistence::EPersistenceError::ENCODE, encoded.error().field
                    });
                auto frozen = std::make_shared<const std::string>(std::move(*encoded));
                return persistence::EncodedArtifact{
                    cxx::SharedBytes<>::fromOwner(frozen, std::as_bytes(std::span(*frozen)))
                };
            }

        private:
            std::shared_ptr<const CompiledFlow> compiled_;
        };
    }
    persistence::PersistenceResult<persistence::DerivedArtifact> captureFlowArtifact(
        std::shared_ptr<const CompiledFlow> compiled
    )
    {
        const bool is_invalid = !compiled || !compiled->source || !compiled->artifact || compiled->bytes.empty();
        if (is_invalid)
            return cxx::unexpected(persistence::PersistenceFailure{persistence::EPersistenceError::INVALID_ARGUMENT});
        return persistence::DerivedArtifact{
            contracts::CodeLease::builtin(),
            {compiled->key.content, compiled->source->id,
             std::string(lux::script::ScriptArtifactAsset::canonical_name), 1,
             lux::script::ScriptArtifactAsset::primary_magic},
            compiled->bytes, std::make_shared<const FlowArtifactSource>(compiled)
        };
    }
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
