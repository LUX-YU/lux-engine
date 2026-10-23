#include <lux/engine/rendering/Initialization.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>

#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <Eigen/Geometry>
#include <cstring>
#include <map>

namespace lux::rendering
{
    using namespace render;
    namespace loading = process::asset_loading;

    namespace
    {
        constexpr error::ErrorDescriptor AssetLoadError{
            "lux.rendering.asset_load",
            "Asset loading stage {0}, storage error {1}, decode error {2}",
            error::ERecovery::NEEDS_INPUT,
            {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
        };
        constexpr auto kAssetLoad = error::errorId(AssetLoadError.name);

        RenderError assetError(const loading::AssetLoadFailure& failure) noexcept
        {
            return {
                kAssetLoad,
                {static_cast<std::uint64_t>(failure.code),
                 static_cast<std::uint64_t>(failure.storage_error),
                 static_cast<std::uint64_t>(failure.decode.code)}
            };
        }

        template <class T> struct AssetRecord final
        {
            std::optional<T> value;
            RenderError error;
            std::size_t references = 1;
            bool loading = true;
            bool releasing = false;
        };

        struct PackedVertex final
        {
            float position[3];
            float uv[2];
            float normal[3];
            float tangent[4];
        };

        struct PackedMesh final
        {
            MeshDescription description;
            std::vector<std::byte> bytes;
        };

        PackedMesh packMesh(const rdesc::Mesh& mesh) noexcept
        {
            PackedMesh packed;
            auto& description = packed.description;
            description.vertex_stride = sizeof(PackedVertex);
            description.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
            description.index_offset = mesh.vertices.size() * sizeof(PackedVertex);
            description.index_count = static_cast<std::uint32_t>(mesh.indices.size());
            description.attributes = {
                {0, offsetof(PackedVertex, position), rdesc::ETextureFormat::RGB32_SFLOAT},
                {1, offsetof(PackedVertex, uv), rdesc::ETextureFormat::RG32_SFLOAT},
                {2, offsetof(PackedVertex, normal), rdesc::ETextureFormat::RGB32_SFLOAT},
                {3, offsetof(PackedVertex, tangent), rdesc::ETextureFormat::RGBA32_SFLOAT}
            };
            description.submeshes = {{0, description.index_count, 0}};
            if (mesh.bounds)
            {
                const auto center = mesh.bounds->center();
                const auto extent = mesh.bounds->halfExtents();
                description.bounds = {{center.x(), center.y(), center.z(), 1}, {extent.x(), extent.y(), extent.z(), 0}};
            }
            packed.bytes.resize(description.index_offset + mesh.indices.size() * sizeof(std::uint32_t));
            auto* vertices = reinterpret_cast<PackedVertex*>(packed.bytes.data());
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
            {
                const auto& input = mesh.vertices[i];
                auto& output = vertices[i];
                std::memcpy(output.position, input.position.data(), sizeof(output.position));
                std::memcpy(output.uv, input.uv.data(), sizeof(output.uv));
                std::memcpy(output.normal, input.normal.data(), sizeof(output.normal));
                std::memcpy(output.tangent, input.tangent.data(), 3 * sizeof(float));
                output.tangent[3] = input.normal.cross(input.tangent).dot(input.bitangent) < 0 ? -1.f : 1.f;
            }
            std::memcpy(
                packed.bytes.data() + description.index_offset,
                mesh.indices.data(),
                mesh.indices.size() * sizeof(std::uint32_t)
            );
            return packed;
        }

        template <class T> RenderError taskError(const execution::TTaskResult<T, RenderError>& result) noexcept
        {
            if (result)
            {
                return {};
            }
            if (const auto* domain = result.error().domainFailure())
            {
                return *domain;
            }
            return RenderError{result.error().isCancelled() ? kCommandCancelled : kRuntimeState};
        }
    } // namespace

    struct RenderAssets::Impl final
    {
        using Meshes = std::map<RenderAssetVersion, AssetRecord<RenderMesh>>;
        using Textures = std::map<RenderAssetVersion, AssetRecord<RenderTexture>>;
        using Materials = std::map<RenderAssetVersion, AssetRecord<std::shared_ptr<const asset::MaterialAsset>>>;

        Impl(RenderExecution& rendering, loading::AssetReadPort source, RenderAssetLimits limits) noexcept
            : rendering(rendering), source(std::move(source)), limits(limits), tasks(rendering.execution())
        {
        }

        template <class Asset>
        auto decode(RenderAssetVersion key, loading::AssetReadPort input, asset::AssetVfsView snapshot) noexcept
        {
            return stdexec::let_error(
                loading::loadAsset<Asset>(
                    input ? std::move(input) : source,
                    rendering.execution().cpu(),
                    key.asset,
                    limits.decode,
                    stop.get_token(),
                    std::move(snapshot)
                ),
                [](loading::AssetLoadFailure failure) noexcept { return stdexec::just_error(assetError(failure)); }
            );
        }

        auto uploadTexture(std::shared_ptr<const asset::TextureAsset>& decoded) noexcept
        {
            auto start = stdexec::then(
                stdexec::schedule(rendering.assetScheduler()),
                [this, &decoded]() noexcept
                {
                    const auto& texture = decoded->data();
                    return TextureWrite{
                        texture_storage,
                        texture.info(),
                        {static_cast<const std::byte*>(texture.data()), texture.size()}
                    };
                }
            );
            return stdexec::let_value(
                std::move(start),
                [this, &decoded](TextureWrite& request) noexcept
                {
                    return stdexec::then(
                        rendering.assetClient().request(*create_texture, request),
                        [&decoded](CreateTexture::Response response) noexcept -> RenderResult<RenderTexture>
                        {
                            // Any actual Response ends the CPU source borrow; adoption need not retain pixels.
                            const auto info = decoded->data().info();
                            decoded.reset();
                            if (response.error.type != 0)
                            {
                                return cxx::unexpected(response.error);
                            }
                            const auto format = rdesc::textureImageFormat(info.pixel_format);
                            return RenderTexture{
                                response.texture,
                                {static_cast<std::uint32_t>(info.width),
                                 static_cast<std::uint32_t>(info.height),
                                 info.mip_count,
                                 info.layers,
                                 format}
                            };
                        }
                    );
                }
            );
        }

        auto uploadMesh(PackedMesh& packed) noexcept
        {
            auto start = stdexec::then(
                stdexec::schedule(rendering.assetScheduler()),
                [this, &packed]() noexcept { return MeshWrite{mesh_storage, packed.description, packed.bytes}; }
            );
            return stdexec::let_value(
                std::move(start),
                [this, &packed](MeshWrite& request) noexcept
                {
                    return stdexec::then(
                        rendering.assetClient().request(*create_mesh, request),
                        [&packed](CreateMesh::Response response) noexcept -> RenderResult<RenderMesh>
                        {
                            std::vector<std::byte>{}.swap(packed.bytes);
                            if (response.error.type != 0)
                            {
                                return cxx::unexpected(response.error);
                            }
                            return RenderMesh{response.mesh, std::move(packed.description)};
                        }
                    );
                }
            );
        }

        template <class Map, class Factory>
        RenderResult<void> acquire(Map& records, RenderAssetVersion key, Factory factory) noexcept
        {
            if (closing || key.asset.isNull())
            {
                return cxx::unexpected(RenderError{kRuntimeState});
            }
            auto found = records.find(key);
            if (found != records.end())
            {
                if (found->second.releasing)
                {
                    return cxx::unexpected(RenderError{kRuntimeCapacity});
                }
                ++found->second.references;
                return {};
            }
            if (in_flight == limits.in_flight)
            {
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            auto [entry, inserted] = records.try_emplace(key);
            ++in_flight;
            auto task = tasks.submit(
                {.name = "Load render asset", .category = "render.asset"},
                [this, key, &records, factory = std::move(factory)](execution::TaskReporter) mutable noexcept
                {
                    auto result = execution::taskResult(factory());
                    return stdexec::then(
                        stdexec::continues_on(std::move(result), rendering.execution().mainScheduler()),
                        [this, key, &records](auto result) noexcept
                        {
                            --in_flight;
                            auto& record = records.find(key)->second;
                            record.loading = false;
                            record.error = taskError(result);
                            if (result)
                            {
                                record.value.emplace(std::move(*result));
                            }
                            ++adoption_revision;
                            if (record.references == 0)
                            {
                                scheduleRelease(records, key);
                            }
                            return execution::taskState(result);
                        }
                    );
                }
            );
            if (!task)
            {
                --in_flight;
                records.erase(entry);
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            return {};
        }

        template <class Map> void scheduleRelease(Map& records, RenderAssetVersion key) noexcept
        {
            if constexpr (std::same_as<Map, Meshes>)
            {
                mesh_releases.push_back(key);
            }
            else if constexpr (std::same_as<Map, Textures>)
            {
                texture_releases.push_back(key);
            }
            else
            {
                records.erase(key);
            }
        }

        template <class C, class Map, class Handle>
        void releaseReady(
            Map& records,
            std::vector<RenderAssetVersion>& pending,
            BoundCommand<C> command,
            Handle handle
        ) noexcept
        {
            // Only zero-reference changes enter this list; live assets are never scanned per frame.
            const auto count = pending.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto key = pending[i];
                const auto found = records.find(key);
                if (found == records.end())
                {
                    continue;
                }
                auto& record = found->second;
                if (record.references != 0 || record.loading || record.releasing)
                {
                    continue;
                }
                if (!record.value)
                {
                    records.erase(found);
                    continue;
                }
                auto input = handle(*record.value);
                record.releasing = true;
                const auto task = tasks.submit(
                    {.name = std::string(C::name), .category = "render.release"},
                    [this, key, &records, command, input](execution::TaskReporter) noexcept
                    {
                        return stdexec::let_value(
                            stdexec::just(input),
                            [this, key, &records, command](typename C::Request& request) noexcept
                            {
                                return stdexec::then(
                                    rendering.mainClient().request(command, request),
                                    [this, key, &records](AckResponse response) noexcept
                                    {
                                        auto& record = records.find(key)->second;
                                        record.releasing = false;
                                        const bool retry = response.error.type == kRuntimeCapacity ||
                                                           response.error.type == kCommandCancelled ||
                                                           response.error.type == kFrameResynchronization;
                                        if (retry)
                                        {
                                            scheduleRelease(records, key);
                                            return execution::ETaskState::FAILED;
                                        }
                                        if (response.error.type != 0)
                                        {
                                            reportRenderFailure(response.error, C::name);
                                        }
                                        records.erase(key);
                                        return response.error.type == 0 ? execution::ETaskState::SUCCEEDED
                                                                        : execution::ETaskState::FAILED;
                                    }
                                );
                            }
                        );
                    }
                );
                if (!task)
                {
                    record.releasing = false;
                    scheduleRelease(records, key);
                }
            }
            pending.erase(pending.begin(), pending.begin() + count);
        }

        template <class Map> void release(Map& records, RenderAssetVersion key) noexcept
        {
            // This is an acquire/release pairing, not an external protocol boundary.
            auto& record = records.find(key)->second;
            if (--record.references == 0 && !record.loading)
            {
                scheduleRelease(records, key);
            }
        }

        template <class Map> static const auto* ready(const Map& records, RenderAssetVersion key) noexcept
        {
            const auto found = records.find(key);
            return found != records.end() && found->second.value ? &*found->second.value : nullptr;
        }

        RenderExecution& rendering;
        loading::AssetReadPort source;
        RenderAssetLimits limits;
        execution::TaskScope tasks;
        std::stop_source stop;
        Meshes meshes;
        Textures textures;
        Materials materials;
        std::vector<RenderAssetVersion> mesh_releases, texture_releases;
        std::optional<BoundCommand<CreateMesh>> create_mesh;
        std::optional<BoundCommand<CreateTexture>> create_texture;
        std::optional<BoundCommand<ReleaseMesh>> release_mesh;
        std::optional<BoundCommand<ReleaseTexture>> release_texture;
        StorageId mesh_storage, texture_storage;
        std::size_t in_flight = 0;
        std::uint64_t adoption_revision = 0;
        bool closing = false;
    };

    RenderAssets::RenderAssets(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderAssets::~RenderAssets() noexcept
    {
        if (!settled())
        {
            std::terminate();
        }
    }

    RenderAssets::CreateResult RenderAssets::create(
        RenderExecution& rendering,
        loading::AssetReadPort source,
        RenderAssetLimits limits
    ) noexcept
    {
        const bool invalid_limits =
            limits.in_flight == 0 || limits.decode.max_image_bytes == 0 || limits.decode.max_decoded_bytes == 0;
        if (!source || invalid_limits)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        if (auto registered = error::ErrorRegistry::instance().registerTypes({&AssetLoadError, 1}); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        auto impl = std::make_unique<Impl>(rendering, std::move(source), limits);
        auto mesh_command = rendering.assetClient().bind<CreateMesh>();
        auto texture_command = rendering.assetClient().bind<CreateTexture>();
        auto mesh_release = rendering.mainClient().bind<ReleaseMesh>();
        auto texture_release = rendering.mainClient().bind<ReleaseTexture>();
        if (!mesh_command || !texture_command || !mesh_release || !texture_release)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        impl->create_mesh = *mesh_command;
        impl->create_texture = *texture_command;
        impl->release_mesh = *mesh_release;
        impl->release_texture = *texture_release;
        const auto storages = detail::requestAtStartup<ListStorages>(rendering, {});
        if (storages.error.type != 0)
        {
            return cxx::unexpected(storages.error);
        }
        for (const auto& storage : storages.storages)
        {
            const bool mesh = storage.name == kMeshStorageType.name();
            const bool texture = storage.name == kTextureStorageType.name();
            if (!mesh && !texture)
            {
                continue;
            }
            auto prepared = detail::requestAtStartup<PrepareStorage>(
                rendering,
                {{GlobalStorageLocation{}, storage.id, "assets"}, {}}
            );
            if (prepared.error.type != 0)
            {
                return cxx::unexpected(prepared.error);
            }
            (mesh ? impl->mesh_storage : impl->texture_storage) = prepared.storage;
        }
        if (impl->mesh_storage.isNull() || impl->texture_storage.isNull())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return std::unique_ptr<RenderAssets>{new RenderAssets(std::move(impl))};
    }

    RenderResult<void> RenderAssets::acquireMesh(
        RenderAssetVersion key,
        loading::AssetReadPort source,
        asset::AssetVfsView snapshot
    ) noexcept
    {
        auto& state = *impl_;
        return state.acquire(
            state.meshes,
            key,
            [&state, key, source = std::move(source), snapshot = std::move(snapshot)]() mutable noexcept
            {
                auto packed = stdexec::then(
                    state.decode<asset::MeshAsset>(key, std::move(source), std::move(snapshot)),
                    [](auto decoded) noexcept { return packMesh(decoded->data()); }
                );
                return stdexec::let_value(
                    std::move(packed),
                    [&state](PackedMesh& input) noexcept { return state.uploadMesh(input); }
                );
            }
        );
    }

    RenderResult<void> RenderAssets::acquireTexture(
        RenderAssetVersion key,
        loading::AssetReadPort source,
        asset::AssetVfsView snapshot
    ) noexcept
    {
        auto& state = *impl_;
        return state.acquire(
            state.textures,
            key,
            [&state, key, source = std::move(source), snapshot = std::move(snapshot)]() mutable noexcept
            {
                return stdexec::let_value(
                    state.decode<asset::TextureAsset>(key, std::move(source), std::move(snapshot)),
                    [&state](auto& decoded) noexcept { return state.uploadTexture(decoded); }
                );
            }
        );
    }

    RenderResult<void> RenderAssets::acquireMaterial(
        RenderAssetVersion key,
        loading::AssetReadPort source,
        asset::AssetVfsView snapshot
    ) noexcept
    {
        auto& state = *impl_;
        return state.acquire(
            state.materials,
            key,
            [&state, key, source = std::move(source), snapshot = std::move(snapshot)]() mutable noexcept
            {
                return stdexec::then(
                    state.decode<asset::MaterialAsset>(key, std::move(source), std::move(snapshot)),
                    [](auto decoded) noexcept -> RenderResult<std::shared_ptr<const asset::MaterialAsset>>
                    { return decoded; }
                );
            }
        );
    }

    void RenderAssets::releaseMesh(RenderAssetVersion key) noexcept
    {
        impl_->release(impl_->meshes, key);
    }

    void RenderAssets::releaseTexture(RenderAssetVersion key) noexcept
    {
        impl_->release(impl_->textures, key);
    }

    void RenderAssets::releaseMaterial(RenderAssetVersion key) noexcept
    {
        impl_->release(impl_->materials, key);
    }

    const RenderMesh* RenderAssets::mesh(RenderAssetVersion key) const noexcept
    {
        return Impl::ready(impl_->meshes, key);
    }

    const RenderTexture* RenderAssets::texture(RenderAssetVersion key) const noexcept
    {
        return Impl::ready(impl_->textures, key);
    }

    const rdesc::MaterialDescription* RenderAssets::material(RenderAssetVersion key) const noexcept
    {
        const auto* value = Impl::ready(impl_->materials, key);
        return value ? &(*value)->data() : nullptr;
    }

    RenderError RenderAssets::failure(RenderAssetVersion key) const noexcept
    {
        if (const auto found = impl_->meshes.find(key); found != impl_->meshes.end())
        {
            return found->second.error;
        }
        if (const auto found = impl_->textures.find(key); found != impl_->textures.end())
        {
            return found->second.error;
        }
        if (const auto found = impl_->materials.find(key); found != impl_->materials.end())
        {
            return found->second.error;
        }
        return {};
    }

    std::uint64_t RenderAssets::revision() const noexcept
    {
        return impl_->adoption_revision;
    }

    void RenderAssets::maintain() noexcept
    {
        auto& state = *impl_;
        state.releaseReady<ReleaseMesh>(
            state.meshes,
            state.mesh_releases,
            *state.release_mesh,
            [](const RenderMesh& value) noexcept { return value.mesh; }
        );
        state.releaseReady<ReleaseTexture>(
            state.textures,
            state.texture_releases,
            *state.release_texture,
            [](const RenderTexture& value) noexcept { return value.texture; }
        );
    }

    void RenderAssets::close() noexcept
    {
        auto& state = *impl_;
        state.closing = true;
        state.stop.request_stop();
        for (auto& [key, value] : state.meshes)
        {
            if (std::exchange(value.references, 0) != 0 && !value.loading)
            {
                state.mesh_releases.push_back(key);
            }
        }
        for (auto& [key, value] : state.textures)
        {
            if (std::exchange(value.references, 0) != 0 && !value.loading)
            {
                state.texture_releases.push_back(key);
            }
        }
        for (auto& [key, value] : state.materials)
        {
            value.references = 0;
        }
        std::erase_if(state.materials, [](const auto& entry) noexcept { return !entry.second.loading; });
        maintain();
    }

    bool RenderAssets::settled() const noexcept
    {
        return impl_->tasks.settled() && impl_->meshes.empty() && impl_->textures.empty() && impl_->materials.empty();
    }
} // namespace lux::rendering
