#pragma once
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <atomic>

namespace lux::editor::sessions::detail
{
    // A result has its own sender-held lifetime. Publishing ready is not permission to destroy the sender.
    template <class T> struct AssetResult final
    {
        enum class EState : std::uint8_t
        {
            PENDING,
            VALUE,
            ERROR,
            CANCELLED
        };
        std::atomic<EState> state{EState::PENDING};
        std::shared_ptr<const T> value;
        lux::process::asset_loading::AssetLoadFailure failure;
    };

    class ResourceTasks final
    {
        struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            ResourceTasks *owner;
            void set_value() && noexcept
            {
                owner->done.store(true, std::memory_order_release);
            }
            stdexec::empty_env get_env() const noexcept
            {
                return {};
            }
        };
        using CloseOperation = stdexec::connect_result_t<lux::process::TaskScopeCloseSender, Receiver>;
        std::unique_ptr<CloseOperation> close_;
        bool started_{};

    public:
        lux::process::TaskScope scope;
        std::atomic<bool> done{};
        ~ResourceTasks() noexcept
        {
            // Cold construction failure has no asynchronous work. Close synchronously, without polling or waiting.
            if (!started_ && !scope.closed())
            {
                auto close = stdexec::connect(scope.close(), Receiver{this});
                stdexec::start(close);
                if (!done.load(std::memory_order_acquire))
                    std::terminate();
            }
            if (!scope.closed())
                std::terminate();
        }
        SceneResult<void> close() noexcept
        {
            if (close_)
                return {};
            try
            {
                close_.reset(new CloseOperation(stdexec::connect(scope.close(), Receiver{this})));
                stdexec::start(*close_);
                return {};
            }
            catch (const std::bad_alloc &)
            {
                return lux::cxx::unexpected(SceneFailure{ESceneError::ALLOCATION_FAILURE});
            }
        }
        template <class T>
        lux::cxx::expected<void, lux::process::ETaskStartError> read(
            lux::process::asset_loading::AssetReadPort port, lux::asset::AssetId id,
            const std::shared_ptr<AssetResult<T>> &result) noexcept
        {
            auto values = stdexec::then(
                lux::process::asset_loading::loadAsset<T>(
                    std::move(port), id, lux::asset::AssetDecodeLimits{16 * 1024 * 1024, 32 * 1024 * 1024, 16}),
                [result](std::shared_ptr<const T> value) noexcept
                {
                    result->value = std::move(value);
                    result->state.store(AssetResult<T>::EState::VALUE, std::memory_order_release);
                });
            auto errors =
                stdexec::upon_error(std::move(values),
                                    [result](lux::process::asset_loading::AssetLoadFailure failure) noexcept
                                    {
                                        result->failure = failure;
                                        result->state.store(AssetResult<T>::EState::ERROR, std::memory_order_release);
                                    });
            auto lifetime = stdexec::upon_stopped(
                std::move(errors), [result]() noexcept
                { result->state.store(AssetResult<T>::EState::CANCELLED, std::memory_order_release); });
            auto admitted = scope.start(std::move(lifetime));
            if (admitted)
                started_ = true;
            return admitted;
        }
    };

    struct ResourceRequest final
    {
        explicit ResourceRequest(ResourceRequestKey key)
            : row{std::move(key)}, mesh_read(std::make_shared<AssetResult<lux::asset::MeshAsset>>()),
              material_read(std::make_shared<AssetResult<lux::asset::MaterialAsset>>())
        {
        }
        SceneResourceRow row;
        std::shared_ptr<AssetResult<lux::asset::MeshAsset>> mesh_read;
        std::shared_ptr<AssetResult<lux::asset::MaterialAsset>> material_read;
        lux::render::RenderRequest<lux::render::MeshUploadedReply> mesh_request;
        lux::render::RenderRequest<lux::render::MaterialUploadedReply> material_request;
        lux::render::RenderRequest<lux::render::ShaderCompiledReply> forward_request, gbuffer_request;
        lux::render::RMeshHandle mesh;
        lux::render::RMaterialHandle material;
        lux::render::ShaderHandle forward, gbuffer;
        bool adopted{};
        std::shared_ptr<std::atomic<bool>> retired_program_consumed;
        void start(ResourceTasks &, lux::process::asset_loading::AssetReadPort) noexcept;
        void acceptReplies() noexcept;
        bool settled() const noexcept;
        void prepareStep(rendering::EditorRenderer &, lux::scene::RenderRuntimeLease &);
        void releaseStep(rendering::EditorRenderer &, lux::scene::RenderRuntimeLease &);
    };

    class SceneResources final
    {
    public:
        SceneResources(SessionId, lux::process::asset_loading::AssetReadPort, rendering::EditorRenderer *, std::size_t);
        ~SceneResources() noexcept;
        SceneResult<void> activate() noexcept;
        SceneResult<bool> prepareUpdate(lux::simulation::ecs::Registry &) noexcept;
        void acknowledgeSnapshot() noexcept;
        void afterPresentation(bool source_update_pending) noexcept;
        SceneResult<void> retry(const ResourceRequestKey &) noexcept;
        SceneResult<std::shared_ptr<const SceneResourceSnapshot>> snapshot(std::uint64_t) const noexcept;
        SceneResult<void> beginClose() noexcept;
        SceneResult<bool> advanceClose() noexcept;
#if defined(LUX_EDITOR_SCENE_TEST_DIAGNOSTICS)
        bool readsSettled() const noexcept;
#endif

    private:
        SessionId session_;
        lux::process::asset_loading::AssetReadPort port_;
        rendering::EditorRenderer *renderer_{};
        lux::scene::RenderRuntimeLease runtime_;
        std::vector<std::unique_ptr<ResourceRequest>> requests_;
        std::size_t capacity_{};
        std::uint64_t sequence_{};
        bool active_{}, closing_{}, closed_{};
        bool pending_change_{true};
        ResourceTasks tasks_;
        SceneResult<void> prepareRetirement(bool all) noexcept;
        lux::render::RenderProgram<> retirement_program_, retirement_progress_;
        bool retirement_pending_{};
    };
} // namespace lux::editor::sessions::detail
