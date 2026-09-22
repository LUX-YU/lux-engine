#pragma once

#include <atomic>
#include <lux/engine/function/render/features/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <stop_token>
#include <unordered_map>

namespace lux::scene::detail
{
template <class T> struct PreparedAsset
{
};
template <> struct PreparedAsset<asset::MeshAsset>
{
    QueryResult<std::shared_ptr<const MeshQueryGeometry>> geometry;
};

template <class T> struct AssetResult final : PreparedAsset<T>
{
    enum class State : std::uint8_t
    {
        PENDING,
        VALUE,
        ERROR,
        CANCELLED
    };
    std::atomic<State> state{State::PENDING};
    std::shared_ptr<const T> value;
    process::asset_loading::AssetLoadFailure failure;
    std::stop_source cancel;
    bool started{};
    ~AssetResult()
    {
        cancel.request_stop();
    }
};

struct MeshResource;
struct MaterialResource;
struct TextureResource;
template <class Resource> struct ResourceUse final
{
    std::shared_ptr<Resource> resource;
    render::RenderResourceUse use;
};

struct AssetReads final
{
    process::TaskScope &tasks;
    process::asset_loading::AssetReadPort port;
    asset::AssetDecodeLimits limits;
    std::shared_ptr<const void> code;
    std::unordered_map<asset::AssetId, std::weak_ptr<ResourceUse<MeshResource>>> meshes;
    std::unordered_map<asset::AssetId, std::weak_ptr<ResourceUse<MaterialResource>>> materials;
    std::unordered_map<asset::AssetId, std::weak_ptr<ResourceUse<TextureResource>>> textures;

    std::shared_ptr<ResourceUse<MeshResource>> acquireMesh(render::RenderRuntime &, asset::AssetId, bool fresh);
    std::shared_ptr<ResourceUse<MaterialResource>> acquireMaterial(render::RenderRuntime &, asset::AssetId, bool fresh);
    std::shared_ptr<ResourceUse<TextureResource>> acquireTexture(render::RenderRuntime &, asset::AssetId, bool fresh);

    template <class T>
    lux::cxx::expected<void, process::ETaskStartError> read(asset::AssetId id,
                                                            const std::shared_ptr<AssetResult<T>> &output)
    {
        if (output->started)
        {
            return {};
        }
        // No callback borrows the source, RenderSystem, or Registry. The
        // weak adoption destination can disappear before IO completes.
        const std::weak_ptr<AssetResult<T>> weak = output;
        auto values =
            stdexec::then(process::asset_loading::loadAsset<T>(port, id, limits, output->cancel.get_token()),
                          [weak, code = code](std::shared_ptr<const T> value) noexcept {
                              if (auto result = weak.lock())
                              {
                                  if constexpr (std::same_as<T, asset::MeshAsset>)
                                  {
                                      std::vector<Eigen::Vector3f> positions;
                                      positions.reserve(value->data().vertices.size());
                                      for (const auto &vertex : value->data().vertices)
                                      {
                                          positions.push_back(vertex.position);
                                      }
                                      result->geometry = MeshQueryGeometry::build(positions, value->data().indices);
                                  }
                                  result->value = std::move(value);
                                  result->state.store(AssetResult<T>::State::VALUE, std::memory_order_release);
                              }
                          });
        auto errors = stdexec::upon_error(
            std::move(values), [weak, code = code](process::asset_loading::AssetLoadFailure failure) noexcept {
                if (auto result = weak.lock())
                {
                    result->failure = failure;
                    result->state.store(AssetResult<T>::State::ERROR, std::memory_order_release);
                }
            });
        auto task = stdexec::upon_stopped(std::move(errors), [weak, code = code]() noexcept {
            if (auto result = weak.lock())
            {
                result->state.store(AssetResult<T>::State::CANCELLED, std::memory_order_release);
            }
        });
        auto admitted = tasks.start(std::move(task));
        if (admitted)
        {
            output->started = true;
        }
        return admitted;
    }
};

struct MeshResource final
{
    asset::AssetId id;
    RenderAssetStatus row;
    std::shared_ptr<AssetResult<asset::MeshAsset>> read{std::make_shared<AssetResult<asset::MeshAsset>>()};
    render::RenderRequest<render::MeshUploadedReply> mesh_request;
    render::RMeshHandle mesh;
    render::MeshStackOperationIds mesh_ops;
    bool retiring{};
    void acceptReplies() noexcept;
    void prepareStep(render::RenderRuntime &, AssetReads &);
    static bool release(void *, render::RenderControlSession &, std::size_t &, bool) noexcept;
};

struct TextureResource final
{
    asset::AssetId id;
    RenderAssetStatus row;
    std::shared_ptr<AssetResult<asset::TextureAsset>> read{std::make_shared<AssetResult<asset::TextureAsset>>()};
    render::RenderRequest<render::Texture2DCreatedReply> upload;
    render::RTextureHandle handle;
    bool retiring{};
    void acceptReplies() noexcept;
    void prepareStep(render::RenderRuntime &, AssetReads &);
    static bool release(void *, render::RenderControlSession &, std::size_t &, bool) noexcept;
};

struct MaterialResource final
{
    asset::AssetId id;
    RenderAssetStatus row;
    std::shared_ptr<AssetResult<asset::MaterialAsset>> read{std::make_shared<AssetResult<asset::MaterialAsset>>()};
    render::RenderRequest<render::MaterialUploadedReply> material_request;
    render::RenderRequest<render::ShaderCompiledReply> forward_request, gbuffer_request;
    render::RMaterialHandle material;
    render::ShaderHandle forward, gbuffer;
    render::MaterialOperationIds material_ops;
    struct TextureDependency final
    {
        asset::AssetId asset;
        std::shared_ptr<ResourceUse<TextureResource>> use;
    };
    std::vector<TextureDependency> textures;
    bool texture_reads_started{}, retiring{}, fresh{};
    void acceptReplies() noexcept;
    void prepareStep(render::RenderRuntime &, AssetReads &);
    static bool release(void *, render::RenderControlSession &, std::size_t &, bool) noexcept;
};

struct AssetRequest final
{
    std::shared_ptr<std::size_t> live;
    ~AssetRequest()
    {
        if (live)
        {
            --*live;
        }
    }
    RenderAssetStatus row;
    std::shared_ptr<ResourceUse<MeshResource>> mesh_use;
    std::shared_ptr<ResourceUse<MaterialResource>> material_use;
    std::shared_ptr<AssetResult<asset::MeshAsset>> mesh_read;
    render::RMeshHandle mesh;
    render::RMaterialHandle material;
    void acceptReplies() noexcept;
    void prepareStep(render::RenderRuntime &, AssetReads &);
};

struct AssetUse final
{
    std::shared_ptr<AssetRequest> request;
};

struct SceneAssetLifetime final
{
    render::RenderSceneReceipt receipt;
    bool alive{true};
};
} // namespace lux::scene::detail
