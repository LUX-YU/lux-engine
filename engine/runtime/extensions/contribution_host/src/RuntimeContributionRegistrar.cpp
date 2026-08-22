#include <lux/engine/runtime/extensions/RuntimeContributionRegistrar.hpp>

#include <utility>

namespace lux::extensions
{
    namespace
    {
        [[nodiscard]] bool unavailable(
            const bool* finished,
            const ModuleLease& module) noexcept
        {
            return !finished || *finished || !module;
        }
    }

    lux::cxx::expected<void, EContributionDraftError>
    SceneContributionRegistrar::add(
        lux::runtime::SceneContributionDescriptor descriptor)
    {
        if (unavailable(finished_, module_))
        {
            return lux::cxx::unexpected(
                EContributionDraftError::REGISTRAR_FINISHED);
        }
        if (!descriptor.id.isValid() || !descriptor.build)
        {
            return lux::cxx::unexpected(
                EContributionDraftError::INVALID_DESCRIPTOR);
        }
        descriptor.provider = module_->id();
        descriptor.module = module_;
        draft_->scene_contributions.push_back(std::move(descriptor));
        return {};
    }

    lux::cxx::expected<void, EContributionDraftError>
    RenderEffectRegistrar::add(
        lux::runtime::RenderEffectDescriptor descriptor)
    {
        if (unavailable(finished_, module_))
        {
            return lux::cxx::unexpected(
                EContributionDraftError::REGISTRAR_FINISHED);
        }
        if (!descriptor.id.isValid() || !descriptor.factory.create_fn)
        {
            return lux::cxx::unexpected(
                EContributionDraftError::INVALID_DESCRIPTOR);
        }
        descriptor.provider = module_->id();
        descriptor.module = module_;
        draft_->render_effects.push_back(std::move(descriptor));
        return {};
    }

    RuntimeContributionRegistrar::RuntimeContributionRegistrar(
        ModuleLease module) noexcept
        : module_(std::move(module))
        , scene_contributions_(draft_, module_, finished_)
        , render_effects_(draft_, module_, finished_)
        , async_operations_(async_builder_, module_)
    {}

    RuntimeRegistrationDraft RuntimeContributionRegistrar::finish() && noexcept
    {
        finished_ = true;
        draft_.async_operations =
            std::move(async_builder_).compileOperations();
        return std::move(draft_);
    }

    lux::cxx::expected<void, RuntimeCatalogCommitFailure>
    validateRuntimeCatalogs(
        const RuntimeRegistrationDraft& draft,
        RuntimeCatalogSet catalogs)
    {
        if (auto checked = catalogs.scene_contributions.validateBatch(
                draft.scene_contributions);
            !checked)
        {
            return lux::cxx::unexpected(RuntimeCatalogCommitFailure{
                ERuntimeCatalogCommitError::SCENE_CONTRIBUTIONS,
                static_cast<std::uint32_t>(checked.error())});
        }
        if (auto checked = catalogs.render_effects.validateBatch(
                draft.render_effects);
            !checked)
        {
            return lux::cxx::unexpected(RuntimeCatalogCommitFailure{
                ERuntimeCatalogCommitError::RENDER_EFFECTS,
                static_cast<std::uint32_t>(checked.error())});
        }
        return {};
    }

    lux::cxx::expected<void, RuntimeCatalogCommitFailure>
    commitRuntimeCatalogs(
        RuntimeRegistrationDraft&& draft,
        RuntimeCatalogSet catalogs)
    {
        if (auto checked = validateRuntimeCatalogs(draft, catalogs); !checked)
            return checked;

        // All three catalogues are main-thread confined. No other mutation can
        // interleave between validation and these moves, so these calls are
        // infallible unless a catalogue invariant has been broken internally.
        if (auto committed = catalogs.scene_contributions.addBatch(
                std::move(draft.scene_contributions));
            !committed)
        {
            return lux::cxx::unexpected(RuntimeCatalogCommitFailure{
                ERuntimeCatalogCommitError::SCENE_CONTRIBUTIONS,
                static_cast<std::uint32_t>(committed.error())});
        }
        if (auto committed = catalogs.render_effects.addBatch(
                std::move(draft.render_effects));
            !committed)
        {
            return lux::cxx::unexpected(RuntimeCatalogCommitFailure{
                ERuntimeCatalogCommitError::RENDER_EFFECTS,
                static_cast<std::uint32_t>(committed.error())});
        }
        return {};
    }
}
