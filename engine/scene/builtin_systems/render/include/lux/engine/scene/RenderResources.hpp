#pragma once

#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/scene/RenderResourceTypes.hpp>
#include <array>
#include <lux/engine/function/render/features/resources/ResourceHandles.hpp>
#include <variant>

namespace lux::scene
{
    class RenderAssets;
    namespace detail
    {
        struct RenderResourcesTestAccess;
        struct ViewResult;
    }

    enum class ESceneResourceState : std::uint8_t
    {
        QUEUED,
        CREATING,
        ATTACHING,
        READY,
        RELEASING,
        RETIRED,
    };

    struct SceneFeatureAttachment final
    {
        render::FeatureTypeId type{};
        std::uint32_t registered_type{};
        std::vector<std::byte> configuration;
        std::shared_ptr<const void> code;
    };

    struct SceneResourceStatus final
    {
        ESceneResourceState state{ESceneResourceState::QUEUED};
        render::RenderSceneId scene;
        render::RenderError failure;
        std::uint64_t request{};
        render::FeatureTypeId feature{};
    };

    // An observation does not prolong resource use. The original result remains
    // readable after the system and the resource record have gone away.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSceneReceipt final
    {
    public:
        [[nodiscard]] SceneResourceStatus status() const noexcept;

    private:
        friend class RenderResources;
        std::shared_ptr<const SceneResourceStatus> record_;
    };

    enum class ERenderAssetState : std::uint8_t
    {
        UNREFERENCED,
        READING,
        UPLOADING,
        READY,
        FAILED,
        CANCELLED,
        CAPACITY
    };

    struct RenderAssetKey final
    {
        SceneInstanceId instance;
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
        asset::AssetId mesh, material;
        std::uint64_t source_version{}, sequence{};
        friend bool operator==(const RenderAssetKey&, const RenderAssetKey&) = default;
    };

    struct RenderAssetStatus final
    {
        RenderAssetKey key;
        ERenderAssetState state{ERenderAssetState::READING};
        std::variant<
            std::monostate,
            process::asset_loading::AssetLoadFailure,
            process::EExecutionError,
            render::ERenderUploadSubmitError,
            render::RendererFailure>
            failure;
        render::RenderError render_failure;
        std::uint32_t backend_status{};
        asset::AssetId failed_dependency;
    };

    struct RenderAssetLimits final
    {
        std::size_t requests{1024};
        asset::AssetDecodeLimits decode{16 * 1024 * 1024, 32 * 1024 * 1024, 16};
    };

    // Identity belongs to the fixed read-view owner, not an endpoint address. The
    // second word distinguishes immutable overlays within the same source domain.
    struct RenderAssetInput final
    {
        std::array<std::uint64_t, 2> source{};
        std::uint64_t version{};
        process::asset_loading::AssetReadPort reads;
        std::shared_ptr<const void> code;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return (source[0] || source[1]) && bool(reads);
        }
    };

    // The receipt observes asynchronous retirement without retaining the View.
    // Native window owners keep it until CLOSED, including target destruction.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderViewReceipt final
    {
    public:
        [[nodiscard]] ViewObservation status() const noexcept;

    private:
        friend class RenderResources;
        std::shared_ptr<const detail::ViewResult> record_;
    };

    // One owner per Runtime. Business references are explicit; copied IDs and GPU
    // handles never retain. Main completions adopt only the active set, before scene maintenance.
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderResources final
    {
    public:
        using CreateResult = render::RenderResult<std::unique_ptr<RenderResources>>;
        [[nodiscard]] static CreateResult create(
            render::RenderRuntime&,
            process::TaskScope&,
            process::CpuScheduler,
            RenderAssetLimits = {}
        ) noexcept;
        ~RenderResources() noexcept;
        RenderResources(const RenderResources&) = delete;
        RenderResources& operator=(const RenderResources&) = delete;

        [[nodiscard]] render::RenderResult<RenderResourceId>
        requestScene(
            const render::RenderControlSession::CreateSceneConfig&,
            std::vector<SceneFeatureAttachment>
        ) noexcept;
        [[nodiscard]] RenderSceneReceipt sceneReceipt(RenderResourceId) const noexcept;
        [[nodiscard]] render::FeatureHandle sceneFeature(RenderResourceId, render::FeatureTypeId) const noexcept;
        [[nodiscard]] render::RenderResult<RenderResourceId> requestView(RenderResourceId scene, ViewConfig) noexcept;
        [[nodiscard]] render::RenderResult<ViewObservation> observeView(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<RenderViewReceipt> viewReceipt(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<void> requestViewExtent(RenderResourceId, render::PixelExtent) noexcept;
        [[nodiscard]] render::RenderResult<void> setViewOutput(RenderResourceId, ViewStamp) noexcept;
        // Observation only: retain the exact output in the same Main operation
        // before storing it. Repeated observation allocates nothing.
        [[nodiscard]] render::RenderResult<RenderResourceId> viewOutput(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<RenderOutputInfo> outputInfo(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<RenderResourceId> requestMesh(
            const RenderAssetInput&,
            asset::AssetId,
            bool retry_failed = false
        ) noexcept;
        [[nodiscard]] render::RenderResult<RenderResourceId> requestMaterial(
            const RenderAssetInput&,
            asset::AssetId,
            bool retry_failed = false
        ) noexcept;
        [[nodiscard]] render::RenderResult<RenderResourceId> requestTexture(
            const RenderAssetInput&,
            asset::AssetId,
            bool retry_failed = false
        ) noexcept;
        [[nodiscard]] render::RenderResult<void> retain(RenderResourceId) noexcept;
        void release(RenderResourceId) noexcept;
        // Fixes a deduplicated set for prepared updates and backend bindings.
        // The observer kept by this manager does not itself keep the batch in use.
        [[nodiscard]] render::RenderResult<
            render::RenderSubmissionState> capture(std::span<const RenderResourceId>) noexcept;
        [[nodiscard]] render::RenderResult<
            render::RenderSubmissionState> captureTextures(std::span<const render::RTextureHandle>) noexcept;
        [[nodiscard]] render::RenderResult<RenderAssetStatus> status(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<render::RMeshHandle> mesh(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<render::RMaterialHandle> material(RenderResourceId) const noexcept;
        [[nodiscard]] render::RenderResult<render::RTextureHandle> texture(RenderResourceId) const noexcept;
        [[nodiscard]] std::size_t viewCount() const noexcept;
        void beginClose() noexcept;
        // Empty table does not assert that backend deferred destruction or GPU work is complete.
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] bool uses(const render::RenderRuntime&) const noexcept;

    private:
        friend class RenderAssets;
        friend struct detail::RenderResourcesTestAccess;
        struct Impl;
        explicit RenderResources(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
