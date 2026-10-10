#pragma once

#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/function/render/features/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <atomic>
#include <stop_token>
#include <thread>
#include <unordered_map>

namespace lux::scene::detail
{
    template <class T> struct TPreparedAsset
    {};
    template <> struct TPreparedAsset<asset::MeshAsset>
    {
        QueryResult<std::shared_ptr<const MeshQueryGeometry>> geometry;
    };

    template <class T> struct TAssetResult final : TPreparedAsset<T>
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
        process::asset_loading::AssetLoadFailure failure;
        std::stop_source cancel;
        bool started{};
        ~TAssetResult()
        {
            cancel.request_stop();
        }
    };

    enum class EResourceKind : std::uint8_t
    {
        MESH,
        MATERIAL,
        TEXTURE
    };
    struct ResourceKey final
    {
        std::array<std::uint64_t, 2> source;
        std::uint64_t version{};
        asset::AssetId asset;
        EResourceKind kind;
        friend bool operator==(const ResourceKey&, const ResourceKey&) = default;
        struct Hash final
        {
            std::size_t operator()(const ResourceKey& key) const noexcept
            {
                auto hash = std::hash<asset::AssetId>{}(key.asset);
                const auto mix = [&](std::uint64_t value) {
                    hash ^= std::hash<std::uint64_t>{}(value) + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
                };
                mix(key.source[0]);
                mix(key.source[1]);
                mix(key.version);
                mix(std::uint64_t(key.kind));
                return hash;
            }
        };
    };
    struct MeshState final
    {
        std::shared_ptr<TAssetResult<asset::MeshAsset>> read;
        render::TRenderRequest<render::MeshUploadedReply> upload;
        render::RMeshHandle handle;
        render::MeshStackOperationIds operations;
    };
    struct ResourceRecord;
    using TextureRecords = std::unordered_map<render::RTextureHandle, ResourceRecord*>;
    struct TextureState final
    {
        std::shared_ptr<TAssetResult<asset::TextureAsset>> read;
        render::TRenderRequest<render::Texture2DCreatedReply> upload;
        render::RTextureHandle handle;
        TextureRecords::node_type lookup;
    };
    struct MaterialState final
    {
        std::shared_ptr<TAssetResult<asset::MaterialAsset>> read;
        render::TRenderRequest<render::MaterialUploadedReply> upload;
        render::TRenderRequest<render::ShaderCompiledReply> forward_upload, gbuffer_upload;
        render::RMaterialHandle handle;
        render::ShaderHandle forward, gbuffer;
        render::MaterialOperationIds operations;
        // One reference per distinct texture dependency, never per material/entity combination.
        std::array<RenderResourceId, rdesc::MaterialDescription::kMaxTextures> textures{};
    };
    struct AssetState final
    {
        ResourceKey key;
        RenderAssetInput input;
        RenderAssetStatus status;
        std::variant<MeshState, MaterialState, TextureState> payload;
        bool retiring{}, retry_failed{};
    };
    struct SceneState final
    {
        std::shared_ptr<SceneResourceStatus> status;
        render::CreateScenePayload creation;
        std::vector<SceneFeatureAttachment> attachments;
        std::vector<std::pair<render::FeatureTypeId, render::FeatureHandle>> features;
        std::size_t next{};
        render::TRenderRequest<render::SceneCreatedReply> create;
        render::TRenderRequest<render::FeatureAddedReply> attach;
        render::TRenderRequest<render::GenericOkReply> release;
        void fail(render::RenderError, std::uint64_t, render::FeatureTypeId = {}) noexcept;
    };
    struct ViewResult final
    {
        ViewObservation value;
        std::size_t outputs{};
    };
    struct EViewState final
    {
        std::shared_ptr<ViewResult> result;
        RenderResourceId scene, current, pending;
        std::variant<SampledOutput, NativeSurfaceOutput> output;
        ViewStamp desired;
        render::RenderTargetId producer, switch_next;
        render::TRenderRequest<render::ViewCreatedReply> create;
        render::TRenderRequest<render::GenericOkReply> switch_target, release;
        render::TRenderRequest<render::TargetResizedReply> resize;
        std::uint64_t resize_sequence{};
        [[nodiscard]] bool sampled() const noexcept
        {
            return std::holds_alternative<SampledOutput>(output);
        }
    };
    struct OutputState final
    {
        std::shared_ptr<ViewResult> owner;
        RenderOutputInfo info;
        render::RenderSubmissionState production;
        render::RenderSubmissionState::Observer produced;
        render::TRenderRequest<render::TargetReadyReply> create;
        render::TRenderRequest<render::TargetReleasedReply> release;
        std::uint64_t native_window{}, generation{};
        bool sampled{}, admitted{}, retired{};
        TextureRecords::node_type lookup;
    };
    struct ResourceRecord final
    {
        RenderResourceId id;
        std::size_t references{1};
        std::variant<AssetState, SceneState, EViewState, OutputState> payload;
        std::vector<render::RenderSubmissionState::Observer> submissions;
        bool queued{};
        AssetState& asset() noexcept
        {
            return std::get<AssetState>(payload);
        }
        const AssetState& asset() const noexcept
        {
            return std::get<AssetState>(payload);
        }
    };
} // namespace lux::scene::detail

namespace lux::scene
{
    using detail::TAssetResult;
    struct RenderResources::Impl final
    {
        using Key = lux::cxx::SlotKey<RenderResourceTag>;
        using Record = detail::ResourceRecord;
        render::RenderRuntime& runtime;
        process::TaskScope& tasks;
        process::CpuScheduler cpu;
        RenderAssetLimits limits;
        std::uint64_t domain;
        const std::thread::id owner{std::this_thread::get_id()};
        lux::cxx::SlotMap<std::unique_ptr<Record>, RenderResourceTag> records;
        std::unordered_map<detail::ResourceKey, Key, detail::ResourceKey::Hash> cache;
        detail::TextureRecords texture_records;
        std::vector<Key> active, batch;
        std::vector<Record*> capture_records;
        process::CompletionWork work;
        std::shared_ptr<process::CompletionWork::Request> completion;
        bool closing{}, polling{}, backend_stopping{};

        Impl(
            render::RenderRuntime& renderer,
            process::TaskScope& scope,
            process::CpuScheduler scheduler,
            RenderAssetLimits capacity,
            std::uint64_t identity
        )
            : runtime(renderer), tasks(scope), cpu(std::move(scheduler)), limits(capacity), domain(identity),
              work(scope.execution(), this, [](void* value) noexcept { static_cast<Impl*>(value)->adoptCompleted(); }),
              completion(std::make_shared<process::CompletionWork::Request>(work.requester()))
        {}
        static void wake(void* value) noexcept
        {
            static_cast<process::CompletionWork::Request*>(value)->request();
        }
        void adoptCompleted() noexcept;

        void requireOwner() const noexcept;
        [[nodiscard]] Record* find(RenderResourceId, bool referenced = true) const noexcept;
        [[nodiscard]] render::RenderResult<render::RenderSubmissionState> capture() noexcept;
        [[nodiscard]] render::RenderResult<RenderResourceId> request(
            const RenderAssetInput&,
            asset::AssetId,
            detail::EResourceKind,
            bool retry_failed
        ) noexcept;
        void enqueue(Record&, bool wake = true) noexcept;
        void release(RenderResourceId) noexcept;
        void advanceScene(Record&) noexcept;
        [[nodiscard]] render::RenderResult<RenderResourceId> makeOutput(detail::EViewState&) noexcept;
        void advanceView(Record&) noexcept;
        void advanceOutput(Record&) noexcept;
        void accept(Record&) noexcept;
        void prepare(Record&) noexcept;
        [[nodiscard]] bool retire(Record&) noexcept;
        template <class T>
        lux::cxx::expected<void, process::EExecutionError> read(
            const RenderAssetInput& input,
            asset::AssetId id,
            const std::shared_ptr<TAssetResult<T>>& output
        )
        {
            if (output->started)
            {
                return {};
            }
            // No callback borrows the source, RenderSystem, or Registry. The
            // weak adoption destination can disappear before IO completes.
            const std::weak_ptr<TAssetResult<T>> weak = output;
            auto values = stdexec::then(
                process::asset_loading::loadAsset<T>(input.reads, cpu, id, limits.decode, output->cancel.get_token()),
                [weak](std::shared_ptr<const T> value
                ) noexcept -> lux::cxx::expected<void, process::asset_loading::AssetLoadFailure> {
                    if (auto result = weak.lock())
                    {
                        if constexpr (std::same_as<T, asset::MeshAsset>)
                        {
                            std::vector<Eigen::Vector3f> positions;
                            positions.reserve(value->data().vertices.size());
                            for (const auto& vertex : value->data().vertices)
                            {
                                positions.push_back(vertex.position);
                            }
                            result->geometry = MeshQueryGeometry::build(positions, value->data().indices);
                        }
                        result->value = std::move(value);
                        result->state.store(TAssetResult<T>::EState::VALUE, std::memory_order_release);
                    }
                    return {};
                }
            );
            auto task = stdexec::upon_error(
                std::move(values),
                [](process::asset_loading::AssetLoadFailure error
                ) noexcept -> lux::cxx::expected<void, process::asset_loading::AssetLoadFailure> {
                    return lux::cxx::unexpected(error);
                }
            );
            auto admitted = tasks.submit(
                {"Prepare render asset", "render", {}, input.code},
                [sender = std::move(task)](process::TaskReporter) mutable noexcept { return std::move(sender); },
                [weak, completed = work.requester()](auto&& finished) noexcept {
                    if (auto result = weak.lock(); result && !finished)
                    {
                        if (const auto* failure = finished.error().domainFailure())
                        {
                            result->failure = *failure;
                            result->state.store(TAssetResult<T>::EState::ERROR, std::memory_order_release);
                        }
                        else
                            result->state.store(TAssetResult<T>::EState::CANCELLED, std::memory_order_release);
                    }
                    completed.request();
                }
            );
            if (admitted)
            {
                output->started = true;
            }
            if (!admitted)
                return lux::cxx::unexpected(admitted.error());
            return {};
        }
    };
} // namespace lux::scene
