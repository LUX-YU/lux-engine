#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
namespace lux::editor::material
{
    namespace
    {
        class MaterialArtifactSource final : public persistence::IArtifactSource
        {
        public:
            explicit MaterialArtifactSource(std::shared_ptr<const CompiledMaterial> compiled)
                : compiled_(std::move(compiled))
            {}
            persistence::PersistenceResult<persistence::EncodedArtifact> encode(std::stop_token stop) const override
            {
                if (stop.stop_requested())
                    return cxx::unexpected(persistence::PersistenceFailure{persistence::EPersistenceError::CANCELLED});
                auto encoded = lux::material::encodeMaterialSource(*compiled_->source());
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
            std::shared_ptr<const CompiledMaterial> compiled_;
        };
    }
    persistence::PersistenceResult<persistence::DerivedArtifact> captureMaterialArtifact(
        std::shared_ptr<const CompiledMaterial> compiled
    )
    {
        if (!compiled)
            return cxx::unexpected(persistence::PersistenceFailure{persistence::EPersistenceError::INVALID_ARGUMENT});
        return persistence::DerivedArtifact{
            lux::object::CodeLease::builtin(),
            {compiled->key().content, compiled->source()->id, std::string(lux::asset::MaterialAsset::canonical_name), 1,
             lux::asset::MaterialAsset::primary_magic},
            compiled->bytes(), std::make_shared<const MaterialArtifactSource>(compiled)
        };
    }
    persistence::PersistenceResult<persistence::WriteTicket> publishCompiledMaterial(
        persistence::WriteCoordinator& writes,
        persistence::WriteTarget target,
        std::shared_ptr<const CompiledMaterial> compiled
    )
    {
        using namespace persistence;
        if (!compiled)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ARGUMENT});
        return publishEncodedArtifact(writes, std::move(target), EncodedArtifact{compiled->bytes()});
    }
}
