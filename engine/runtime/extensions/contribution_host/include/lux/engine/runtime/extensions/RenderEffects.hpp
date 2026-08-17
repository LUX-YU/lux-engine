#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/OperationTicket.hpp>
#include <lux/engine/core/extension_abi/StableId.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/runtime/extensions/ContributionConfig.hpp>
#include <lux/engine/runtime/extensions/contribution_visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::ecs
{
    class RenderSystem;
    class SceneServices;
}

namespace lux::events
{
    class DomainEvents;
}

namespace lux::render
{
    class FeatureCatalog;
    class RenderControlSession;
}

namespace lux::runtime
{
    class SceneContributionHost;

    enum class ERenderEffectCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_CONTRIBUTION,
        ID_COLLISION,
        INVALID_FACTORY
    };

    enum class ERenderExtractionBuildError : std::uint8_t
    {
        INVALID_CONFIG,
        BUILD_REJECTED,
        DUPLICATE_SUBSYSTEM
    };

    struct RenderExtractionBuildFailure final
    {
        ERenderExtractionBuildError code{
            ERenderExtractionBuildError::BUILD_REJECTED};
        lux::ecs::RenderSubsystemType type{};
    };

    struct RenderEffectBuildContext final
    {
        const lux::ecs::SceneServices& services;
    };

    struct RenderEffectContributionDescriptor final
    {
        RenderEffectContributionDescriptor() = default;
        RenderEffectContributionDescriptor(
            const RenderEffectContributionDescriptor&) = delete;
        RenderEffectContributionDescriptor& operator=(
            const RenderEffectContributionDescriptor&) = delete;
        RenderEffectContributionDescriptor(
            RenderEffectContributionDescriptor&&) noexcept = default;
        RenderEffectContributionDescriptor& operator=(
            RenderEffectContributionDescriptor&&) noexcept = default;

        lux::extensions::ContributionId id;
        std::string display_name;
        lux::render::FeatureFactory factory{};
        std::vector<lux::extensions::ContributionId>
            required_scene_contributions;
        std::uint32_t config_schema_version{0u};
        ContributionConfig default_config;
        lux::cxx::move_only_function<
            lux::cxx::expected<void, RenderExtractionBuildFailure>(
                lux::ecs::RenderSubsystemMutationBatch&,
                const RenderEffectBuildContext&,
                ContributionConfig)>
            build_extraction;
        lux::extensions::ExtensionId provider;
        lux::extensions::ModuleLease module;
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC RenderEffectCatalog final
    {
    public:
        RenderEffectCatalog() = default;
        RenderEffectCatalog(const RenderEffectCatalog&) = delete;
        RenderEffectCatalog& operator=(const RenderEffectCatalog&) = delete;
        RenderEffectCatalog(RenderEffectCatalog&&) noexcept = default;
        RenderEffectCatalog& operator=(RenderEffectCatalog&&) noexcept =
            default;

        [[nodiscard]] lux::cxx::expected<void, ERenderEffectCatalogError> add(
            RenderEffectContributionDescriptor descriptor);
        [[nodiscard]] lux::cxx::expected<void, ERenderEffectCatalogError>
        validateBatch(
            std::span<const RenderEffectContributionDescriptor> descriptors)
            const noexcept;
        [[nodiscard]] lux::cxx::expected<void, ERenderEffectCatalogError>
        addBatch(std::vector<RenderEffectContributionDescriptor> descriptors);
        [[nodiscard]] RenderEffectContributionDescriptor* find(
            lux::extensions::ContributionIdView id) noexcept;
        [[nodiscard]] const RenderEffectContributionDescriptor* find(
            lux::extensions::ContributionIdView id) const noexcept;
        [[nodiscard]] std::span<const RenderEffectContributionDescriptor> all()
            const noexcept;

    private:
        std::vector<RenderEffectContributionDescriptor> descriptors_;
    };

    enum class ERenderEffectTypePhase : std::uint8_t
    {
        UNSEEN,
        REGISTERING,
        READY,
        FAILED
    };

    struct RenderEffectTypeSnapshot final
    {
        ERenderEffectTypePhase phase{ERenderEffectTypePhase::UNSEEN};
        std::uint32_t type_id{0u};
        std::uint32_t op_count{0u};
        lux::render::TypeId ops[16]{};
        lux::render::RenderError error{};
    };

    /// Process-domain merger for render-thread feature-type registration.
    /// Scene hosts share this object, so concurrent first use of one effect
    /// produces one control request and one stable registration result.
    class LUX_RUNTIME_CONTRIBUTION_PUBLIC RenderEffectTypeRegistry final
    {
    public:
        explicit RenderEffectTypeRegistry(lux::render::RenderControlSession& control);
        ~RenderEffectTypeRegistry();
        RenderEffectTypeRegistry(const RenderEffectTypeRegistry&) = delete;
        RenderEffectTypeRegistry& operator=(
            const RenderEffectTypeRegistry&) = delete;

        [[nodiscard]] RenderEffectTypeSnapshot ensure(
            const RenderEffectContributionDescriptor& descriptor) noexcept;
        void clear() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class ERenderEffectActivationPhase : std::uint8_t
    {
        QUEUED,
        REGISTERING_TYPE,
        ADDING_RENDER_INSTANCE,
        INSTALLING_EXTRACTION,
        ACTIVE,
        REMOVING_EXTRACTION,
        REMOVING_RENDER_INSTANCE,
        COMPENSATING,
        INACTIVE
    };

    enum class ERenderEffectActivationError : std::uint8_t
    {
        NONE,
        QUEUE_FULL,
        BYTE_BUDGET_EXHAUSTED,
        STOPPING,
        UNKNOWN_CONTRIBUTION,
        CONFIG_VERSION_MISMATCH,
        MISSING_WORLD_CONTRIBUTION,
        FEATURE_TYPE_REGISTRATION_FAILED,
        FEATURE_CATALOG_CONFLICT,
        ADD_FEATURE_FAILED,
        EXTRACTION_BUILD_FAILED,
        EXTRACTION_INSTALL_FAILED,
        EXTRACTION_REMOVE_FAILED,
        REMOVE_FEATURE_FAILED,
        NOT_ACTIVE,
        COMPENSATION_FAILED
    };

    struct RenderEffectActivationResult final
    {
        lux::extensions::ContributionId contribution;
        std::uint64_t generation{0u};
        bool active{false};
    };

    using RenderEffectOperationTicket = lux::extensions::OperationTicket<
        ERenderEffectActivationPhase,
        ERenderEffectActivationError,
        RenderEffectActivationResult>;

    struct RenderEffectStateChanged final
    {
        lux::extensions::ContributionId contribution;
        std::uint64_t generation{0u};
        bool active{false};
    };

    struct RenderEffectActivationSnapshot final
    {
        lux::extensions::ContributionId contribution;
        ContributionConfig config;
        EActivationPersistence persistence{EActivationPersistence::SCENE};
        lux::extensions::ExtensionId provider;
        std::uint64_t generation{0u};
    };

    struct RenderEffectQueueConfig final
    {
        std::size_t capacity{64u};
        std::size_t byte_budget{1024u * 1024u};
    };

    namespace detail
    {
        struct RenderEffectEndpoint;
    }

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC RenderEffects final
    {
    public:
        RenderEffects() noexcept = default;

        [[nodiscard]] RenderEffectOperationTicket requestEnable(
            lux::extensions::ContributionIdView id,
            ContributionConfig config = {},
            EActivationPersistence persistence =
                EActivationPersistence::SCENE) const;
        [[nodiscard]] RenderEffectOperationTicket requestDisable(
            lux::extensions::ContributionIdView id) const;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class RenderEffectHost;
        explicit RenderEffects(
            std::shared_ptr<detail::RenderEffectEndpoint> endpoint) noexcept;
        std::shared_ptr<detail::RenderEffectEndpoint> endpoint_;
    };

    struct RenderEffectCloseReport final
    {
        std::size_t removed{0u};
        std::size_t failed{0u};
        std::size_t rejected_queued{0u};
        std::size_t pending{0u};

        [[nodiscard]] bool terminal() const noexcept
        {
            return pending == 0u;
        }
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC RenderEffectHost final
    {
    public:
        RenderEffectHost(
            lux::ecs::RenderSystem& render_system,
            lux::ecs::SceneServices& services,
            lux::render::RenderSceneId scene,
            lux::render::RenderControlSession& control,
            lux::render::FeatureCatalog& feature_catalog,
            RenderEffectCatalog& catalog,
            RenderEffectTypeRegistry& type_registry,
            const SceneContributionHost* scene_contributions = nullptr,
            lux::events::DomainEvents* events = nullptr,
            lux::cxx::move_only_function<void()> progress = {},
            RenderEffectQueueConfig queue = {});
        ~RenderEffectHost() noexcept;
        RenderEffectHost(const RenderEffectHost&) = delete;
        RenderEffectHost& operator=(const RenderEffectHost&) = delete;

        [[nodiscard]] RenderEffects facade() const noexcept;
        [[nodiscard]] std::size_t processSafePoint(
            std::size_t budget = 16u) noexcept;
        [[nodiscard]] bool active(
            lux::extensions::ContributionIdView id) const noexcept;
        [[nodiscard]] std::vector<RenderEffectActivationSnapshot>
        activationSnapshot() const;
        [[nodiscard]] RenderEffectCloseReport close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
