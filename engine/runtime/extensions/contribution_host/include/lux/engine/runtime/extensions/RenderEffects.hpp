#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/OperationTicket.hpp>
#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/RenderEffectId.hpp>
#include <lux/engine/scene/SceneFeatureId.hpp>
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
        DUPLICATE_EFFECT,
        ID_COLLISION,
        INVALID_FACTORY,
        INVALID_SCENE_FEATURE_DEPENDENCY,
        DUPLICATE_SCENE_FEATURE_DEPENDENCY,
        SCENE_FEATURE_DEPENDENCY_ID_COLLISION
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

    struct RenderEffectDescriptor final
    {
        RenderEffectDescriptor() = default;
        RenderEffectDescriptor(
            const RenderEffectDescriptor&) = delete;
        RenderEffectDescriptor& operator=(
            const RenderEffectDescriptor&) = delete;
        RenderEffectDescriptor(
            RenderEffectDescriptor&&) noexcept = default;
        RenderEffectDescriptor& operator=(
            RenderEffectDescriptor&&) noexcept = default;

        lux::render::RenderEffectId id;
        std::string display_name;
        lux::render::FeatureFactory factory{};
        std::vector<lux::scene::SceneFeatureId> required_scene_features;
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
            RenderEffectDescriptor descriptor);
        [[nodiscard]] lux::cxx::expected<void, ERenderEffectCatalogError>
        validateBatch(
            std::span<const RenderEffectDescriptor> descriptors)
            const noexcept;
        [[nodiscard]] lux::cxx::expected<void, ERenderEffectCatalogError>
        addBatch(std::vector<RenderEffectDescriptor> descriptors);
        [[nodiscard]] RenderEffectDescriptor* find(
            lux::render::RenderEffectIdView id) noexcept;
        [[nodiscard]] const RenderEffectDescriptor* find(
            lux::render::RenderEffectIdView id) const noexcept;
        [[nodiscard]] std::span<const RenderEffectDescriptor> all()
            const noexcept;

    private:
        std::vector<RenderEffectDescriptor> descriptors_;
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
            const RenderEffectDescriptor& descriptor) noexcept;
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
        UNKNOWN_EFFECT,
        CONFIG_VERSION_MISMATCH,
        MISSING_SCENE_FEATURE,
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
        lux::render::RenderEffectId effect;
        std::uint64_t generation{0u};
        bool active{false};
    };

    using RenderEffectOperationTicket = lux::extensions::OperationTicket<
        ERenderEffectActivationPhase,
        ERenderEffectActivationError,
        RenderEffectActivationResult>;

    struct RenderEffectStateChanged final
    {
        lux::render::RenderEffectId effect;
        std::uint64_t generation{0u};
        bool active{false};
    };

    struct RenderEffectActivationSnapshot final
    {
        lux::render::RenderEffectId effect;
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
            lux::render::RenderEffectIdView id,
            ContributionConfig config = {},
            EActivationPersistence persistence =
                EActivationPersistence::SCENE) const;
        [[nodiscard]] RenderEffectOperationTicket requestDisable(
            lux::render::RenderEffectIdView id) const;
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
            const SceneContributionHost* scene_features = nullptr,
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
            lux::render::RenderEffectIdView id) const noexcept;
        [[nodiscard]] std::vector<RenderEffectActivationSnapshot>
        activationSnapshot() const;
        [[nodiscard]] RenderEffectCloseReport close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
