#pragma once
/**
 * @file RuntimeContributionRegistrar.hpp
 * @brief Unpublished registration transaction for one runtime module.
 *
 * A DLL callback can only append host-owned descriptors to this draft. It
 * cannot mutate a live catalogue, create a World object, or contact the render
 * thread. The composition root installs the async bundle first and publishes
 * the three catalogue batches at a main-thread safe point afterwards.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/RenderEffects.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>
#include <lux/engine/runtime/extensions/contribution_visibility.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lux::extensions
{
    enum class EContributionDraftError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        REGISTRAR_FINISHED
    };

    struct RuntimeRegistrationDraft final
    {
        RuntimeRegistrationDraft() = default;
        RuntimeRegistrationDraft(const RuntimeRegistrationDraft&) = delete;
        RuntimeRegistrationDraft& operator=(const RuntimeRegistrationDraft&) =
            delete;
        RuntimeRegistrationDraft(RuntimeRegistrationDraft&&) noexcept = default;
        RuntimeRegistrationDraft& operator=(RuntimeRegistrationDraft&&) noexcept =
            default;

        [[nodiscard]] lux::exec::AsyncOperationBundle takeAsyncOperations()
            noexcept
        {
            return std::move(async_operations);
        }

        std::vector<lux::runtime::SceneContributionDescriptor>
            scene_contributions;
        std::vector<lux::runtime::RenderEffectDescriptor>
            render_effects;
        lux::exec::AsyncOperationBundle async_operations;
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC SceneContributionRegistrar final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, EContributionDraftError> add(
            lux::runtime::SceneContributionDescriptor descriptor);

    private:
        friend class RuntimeContributionRegistrar;
        SceneContributionRegistrar(
            RuntimeRegistrationDraft& draft,
            ModuleLease module,
            const bool& finished) noexcept
            : draft_(&draft), module_(std::move(module)), finished_(&finished)
        {}

        RuntimeRegistrationDraft* draft_{nullptr};
        ModuleLease module_;
        const bool* finished_{nullptr};
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC RenderEffectRegistrar final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, EContributionDraftError> add(
            lux::runtime::RenderEffectDescriptor descriptor);

    private:
        friend class RuntimeContributionRegistrar;
        RenderEffectRegistrar(
            RuntimeRegistrationDraft& draft,
            ModuleLease module,
            const bool& finished) noexcept
            : draft_(&draft), module_(std::move(module)), finished_(&finished)
        {}

        RuntimeRegistrationDraft* draft_{nullptr};
        ModuleLease module_;
        const bool* finished_{nullptr};
    };

    class AsyncOperationRegistrar final
    {
    public:
        template <lux::exec::AsyncOperation Operation, class Handler>
        [[nodiscard]] auto addOperation(
            Handler handler,
            lux::exec::AsyncOperationRegistrationOptions options = {},
            lux::exec::AsyncOperationQueueConfig queue = {})
        {
            options.module_lease = module_;
            return builder_->addOperation<Operation>(
                std::move(handler),
                std::move(options),
                queue);
        }

    private:
        friend class RuntimeContributionRegistrar;
        AsyncOperationRegistrar(
            lux::exec::AsyncRuntimeBuilder& builder,
            ModuleLease module) noexcept
            : builder_(&builder), module_(std::move(module))
        {}

        lux::exec::AsyncRuntimeBuilder* builder_{nullptr};
        ModuleLease module_;
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC RuntimeContributionRegistrar final
    {
    public:
        explicit RuntimeContributionRegistrar(ModuleLease module) noexcept;
        RuntimeContributionRegistrar(const RuntimeContributionRegistrar&) =
            delete;
        RuntimeContributionRegistrar& operator=(
            const RuntimeContributionRegistrar&) = delete;

        [[nodiscard]] SceneContributionRegistrar& sceneContributions() noexcept
        {
            return scene_contributions_;
        }
        [[nodiscard]] RenderEffectRegistrar& renderEffects() noexcept
        {
            return render_effects_;
        }
        [[nodiscard]] AsyncOperationRegistrar& asyncOperations() noexcept
        {
            return async_operations_;
        }

        [[nodiscard]] RuntimeRegistrationDraft finish() && noexcept;

    private:
        ModuleLease module_;
        bool finished_{false};
        RuntimeRegistrationDraft draft_;
        lux::exec::AsyncRuntimeBuilder async_builder_;
        SceneContributionRegistrar scene_contributions_;
        RenderEffectRegistrar render_effects_;
        AsyncOperationRegistrar async_operations_;
    };

    struct RuntimeCatalogSet final
    {
        lux::ecs::ComponentTypeCatalog& components;
        lux::runtime::SceneContributionCatalog& scene_contributions;
        lux::runtime::RenderEffectCatalog& render_effects;
    };

    enum class ERuntimeCatalogCommitError : std::uint8_t
    {
        SCENE_CONTRIBUTIONS,
        RENDER_EFFECTS
    };

    struct RuntimeCatalogCommitFailure final
    {
        ERuntimeCatalogCommitError code{
            ERuntimeCatalogCommitError::SCENE_CONTRIBUTIONS};
        std::uint32_t detail{0u};
    };

    [[nodiscard]] LUX_RUNTIME_CONTRIBUTION_PUBLIC lux::cxx::expected<
        void,
        RuntimeCatalogCommitFailure>
    validateRuntimeCatalogs(
        const RuntimeRegistrationDraft& draft,
        RuntimeCatalogSet catalogs);

    /// Validate every catalogue before mutating any of them. The caller must
    /// have already installed draft.async_operations on the coordinator.
    [[nodiscard]] LUX_RUNTIME_CONTRIBUTION_PUBLIC lux::cxx::expected<
        void,
        RuntimeCatalogCommitFailure>
    commitRuntimeCatalogs(
        RuntimeRegistrationDraft&& draft,
        RuntimeCatalogSet catalogs);
}
