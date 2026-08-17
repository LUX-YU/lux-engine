#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>

#include <lux/engine/function/render/client/features/terrain/TerrainOperation.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>
#include <lux/engine/resource/terrain/TerrainTile.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_set>

namespace lux::runtime::detail
{
    enum class ESceneGeometryPrepareDomain : std::uint8_t
    {
        CLASSIC_MESH,
        TERRAIN
    };

    struct SceneGeometryPrepareDomainState final
    {
        SceneGeometryPrepareDomainConfig config;
        std::size_t active_requests{0u};
        std::size_t active_bytes{0u};
        std::size_t request_high_water{0u};
        std::size_t byte_high_water{0u};
    };

    struct SceneGeometryPrepareControl final
        : public std::enable_shared_from_this<SceneGeometryPrepareControl>
    {
        explicit SceneGeometryPrepareControl(
            SceneGeometryPrepareConfig value) noexcept
            : classic_mesh{value.classic_mesh}, terrain{value.terrain}
        {}

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<SceneGeometryPrepareReservation>,
            lux::exec::EAsyncSubmitError>
        reserve(
            std::uint64_t client_generation,
            ESceneGeometryPrepareDomain domain,
            std::size_t bytes) noexcept;

        [[nodiscard]] bool accepts(
            std::uint64_t client_generation) const noexcept
        {
            std::lock_guard lock{mutex};
            return !closing && generation == client_generation;
        }

        [[nodiscard]] std::uint64_t currentGeneration() const noexcept
        {
            std::lock_guard lock{mutex};
            return !closing ? generation : 0u;
        }

        [[nodiscard]] bool isClosing() const noexcept
        {
            std::lock_guard lock{mutex};
            return closing;
        }

        void closeAdmission() noexcept
        {
            std::lock_guard lock{mutex};
            if (closing)
                return;
            closing = true;
            ++generation;
            if (generation == 0u)
                ++generation;
        }

        void release(
            ESceneGeometryPrepareDomain domain,
            std::size_t bytes) noexcept
        {
            std::lock_guard lock{mutex};
            auto& state = domain == ESceneGeometryPrepareDomain::CLASSIC_MESH
                ? classic_mesh
                : terrain;
            if (state.active_requests == 0u || state.active_bytes < bytes)
                std::abort();
            --state.active_requests;
            state.active_bytes -= bytes;
        }

        [[nodiscard]] SceneGeometryPrepareSnapshot snapshot() const noexcept
        {
            std::lock_guard lock{mutex};
            return {
                closing,
                {classic_mesh.active_requests,
                 classic_mesh.active_bytes,
                 classic_mesh.request_high_water,
                 classic_mesh.byte_high_water},
                {terrain.active_requests,
                 terrain.active_bytes,
                 terrain.request_high_water,
                 terrain.byte_high_water}};
        }

        mutable std::mutex mutex;
        bool closing{false};
        std::uint64_t generation{1u};
        SceneGeometryPrepareDomainState classic_mesh;
        SceneGeometryPrepareDomainState terrain;
    };

    struct SceneGeometryPrepareReservation final
    {
        SceneGeometryPrepareReservation(
            std::shared_ptr<SceneGeometryPrepareControl> owner_value,
            ESceneGeometryPrepareDomain domain_value,
            std::size_t bytes_value) noexcept
            : owner(std::move(owner_value)),
              domain(domain_value),
              bytes(bytes_value)
        {}

        ~SceneGeometryPrepareReservation()
        {
            if (owner)
                owner->release(domain, bytes);
        }

        std::shared_ptr<SceneGeometryPrepareControl> owner;
        ESceneGeometryPrepareDomain domain{
            ESceneGeometryPrepareDomain::CLASSIC_MESH};
        std::size_t bytes{0u};
    };

    lux::cxx::expected<
        std::shared_ptr<SceneGeometryPrepareReservation>,
        lux::exec::EAsyncSubmitError>
    SceneGeometryPrepareControl::reserve(
        std::uint64_t client_generation,
        ESceneGeometryPrepareDomain domain,
        std::size_t bytes) noexcept
    {
        std::lock_guard lock{mutex};
        if (closing || generation != client_generation)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::FEATURE_CLOSING);
        }
        auto& state = domain == ESceneGeometryPrepareDomain::CLASSIC_MESH
            ? classic_mesh
            : terrain;
        if (state.active_requests >= state.config.capacity)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::QUEUE_FULL);
        }
        if (bytes == 0u || bytes > state.config.byte_budget ||
            state.active_bytes > state.config.byte_budget - bytes)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        ++state.active_requests;
        state.active_bytes += bytes;
        state.request_high_water = std::max(
            state.request_high_water, state.active_requests);
        state.byte_high_water = std::max(
            state.byte_high_water, state.active_bytes);
        return std::make_shared<SceneGeometryPrepareReservation>(
            shared_from_this(), domain, bytes);
    }
} // namespace lux::runtime::detail

namespace lux::runtime
{
    namespace ex = stdexec;

    namespace
    {
        constexpr std::size_t kPrepareAllocationOverhead = 64u * 1024u;

        [[nodiscard]] bool checkedAdd(
            std::size_t left,
            std::size_t right,
            std::size_t& result) noexcept
        {
            if (left > std::numeric_limits<std::size_t>::max() - right)
                return false;
            result = left + right;
            return true;
        }

        [[nodiscard]] bool checkedMultiply(
            std::size_t left,
            std::size_t right,
            std::size_t& result) noexcept
        {
            if (left != 0u &&
                right > std::numeric_limits<std::size_t>::max() / left)
            {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] std::uint32_t littleU32(
            const std::byte* bytes) noexcept
        {
            return static_cast<std::uint32_t>(bytes[0]) |
                (static_cast<std::uint32_t>(bytes[1]) << 8u) |
                (static_cast<std::uint32_t>(bytes[2]) << 16u) |
                (static_cast<std::uint32_t>(bytes[3]) << 24u);
        }

        [[nodiscard]] lux::cxx::expected<
            std::size_t,
            lux::exec::EAsyncSubmitError>
        classicMeshWorksetBytes(
            std::span<const std::byte> bytes) noexcept
        {
            constexpr std::size_t kHeaderBytes = 12u;
            constexpr std::size_t kEncodedRowBytes = 88u;
            if (bytes.size() < kHeaderBytes)
                return std::max(bytes.size(), std::size_t{1u});
            const auto magic = littleU32(bytes.data());
            const auto version = littleU32(bytes.data() + 4u);
            const auto count = littleU32(bytes.data() + 8u);
            if (magic != lux::classic_mesh::kClassicMeshBatchBlobMagic ||
                version !=
                    lux::classic_mesh::kClassicMeshBatchSchemaVersion ||
                count == 0u)
            {
                return std::max(bytes.size(), std::size_t{1u});
            }
            std::size_t encoded_rows = 0u;
            std::size_t expected_bytes = 0u;
            if (!checkedMultiply(count, kEncodedRowBytes, encoded_rows) ||
                !checkedAdd(kHeaderBytes, encoded_rows, expected_bytes) ||
                expected_bytes != bytes.size())
            {
                return std::max(bytes.size(), std::size_t{1u});
            }
            // Peak includes the still-live encoded bytes, decoded rows, the
            // preallocated render wire, worst-case unique mesh/material
            // vectors, and conservative unordered-set node/bucket storage.
            constexpr std::size_t kPerRowPreparedBytes =
                sizeof(lux::classic_mesh::ClassicMeshBatchInstanceV1) +
                sizeof(lux::render::RenderClusterWireInstance) +
                4u * sizeof(lux::asset::asset_id_t) + 128u;
            std::size_t prepared_rows = 0u;
            std::size_t peak = 0u;
            if (!checkedMultiply(count, kPerRowPreparedBytes, prepared_rows) ||
                !checkedAdd(bytes.size(), prepared_rows, peak) ||
                !checkedAdd(peak, kPrepareAllocationOverhead, peak))
            {
                return lux::cxx::unexpected(
                    lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
            }
            return peak;
        }

        [[nodiscard]] lux::cxx::expected<
            std::size_t,
            lux::exec::EAsyncSubmitError>
        terrainWorksetBytes(std::span<const std::byte> bytes) noexcept
        {
            // LXTT v1 is fixed-layout.  During packing, encoded bytes, decoded
            // arrays and the upload wire coexist; three encoded-size copies
            // plus a small allocator/header allowance is conservative.
            std::size_t copies = 0u;
            std::size_t peak = 0u;
            if (!checkedMultiply(bytes.size(), 3u, copies) ||
                !checkedAdd(copies, kPrepareAllocationOverhead, peak))
            {
                return lux::cxx::unexpected(
                    lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
            }
            return std::max(peak, std::size_t{1u});
        }

        template <typename T>
        using SceneGeometryPrepareExp =
            lux::cxx::expected<T, SceneGeometryPrepareFailure>;

        using ClassicResult = SceneGeometryPrepareExp<PreparedClassicMeshBatch>;
        using TerrainResult = SceneGeometryPrepareExp<PreparedTerrainTile>;

        [[nodiscard]] SceneGeometryPrepareFailure failure(
            ESceneGeometryPrepareError code,
            std::string detail) noexcept
        {
            return {code, std::move(detail)};
        }

        [[nodiscard]] bool matchesClassicMesh(
            const lux::entity_scene::ContentBlobRef& reference,
            std::span<const std::byte> bytes) noexcept
        {
            return reference.valid() &&
                reference.type.name() ==
                    lux::classic_mesh::kClassicMeshBatchContentTypeName &&
                reference.schema_version ==
                    lux::classic_mesh::kClassicMeshBatchSchemaVersion &&
                lux::entity_scene::makeContentBlobId(
                    reference.type,
                    reference.schema_version,
                    bytes) == reference.id;
        }

        [[nodiscard]] bool matchesTerrain(
            const lux::entity_scene::ContentBlobRef& reference,
            std::span<const std::byte> bytes) noexcept
        {
            return reference.valid() &&
                reference.type.name() ==
                    lux::terrain::kTerrainTileContentTypeName &&
                reference.schema_version ==
                    lux::terrain::kTerrainTileSchemaVersion &&
                lux::entity_scene::makeContentBlobId(
                    reference.type,
                    reference.schema_version,
                    bytes) == reference.id;
        }

        [[nodiscard]] ClassicResult prepareClassicMesh(
            PrepareClassicMeshBatch request) noexcept
        {
            if (request.content.empty() || request.request_generation == 0u)
            {
                return lux::cxx::unexpected(failure(
                    ESceneGeometryPrepareError::INVALID_REQUEST,
                    "Classic Mesh preparation request is empty"));
            }
            if (!matchesClassicMesh(
                    request.reference, request.content.view()))
            {
                return lux::cxx::unexpected(failure(
                    ESceneGeometryPrepareError::CONTENT_MISMATCH,
                    "Classic Mesh content type or schema does not match"));
            }
            auto decoded = lux::classic_mesh::decodeClassicMeshBatchBlob(
                request.content.view());
            if (!decoded || decoded->instances.empty())
            {
                return lux::cxx::unexpected(failure(
                    ESceneGeometryPrepareError::DECODE_FAILED,
                    decoded
                        ? "Classic Mesh batch contains no instances"
                        : decoded.error().detail));
            }

            PreparedClassicMeshBatch result;
            result.request_generation = request.request_generation;
            result.decoded = std::make_shared<
                lux::classic_mesh::ClassicMeshBatchBlobV1>(
                    std::move(*decoded));
            result.wire = std::make_shared<std::vector<
                lux::render::RenderClusterWireInstance>>(
                    result.decoded->instances.size());
            std::unordered_set<lux::asset::asset_id_t> meshes;
            std::unordered_set<lux::asset::asset_id_t> materials;
            meshes.reserve(result.decoded->instances.size());
            materials.reserve(result.decoded->instances.size());
            for (const auto& row : result.decoded->instances)
            {
                if (meshes.insert(row.mesh_asset).second)
                    result.mesh_assets.push_back(row.mesh_asset);
                if (!row.material_asset.is_nil() &&
                    materials.insert(row.material_asset).second)
                {
                    result.material_assets.push_back(row.material_asset);
                }
            }
            return result;
        }

        [[nodiscard]] std::shared_ptr<std::vector<std::byte>>
        makeTerrainWireData(const lux::terrain::TerrainTileBlobV1& tile)
        {
            const auto total =
                sizeof(lux::render::TerrainWirePageDataHeader) +
                tile.heights.size() * sizeof(std::uint16_t) +
                tile.weight_planes[0].size() +
                tile.weight_planes[1].size() + tile.holes.size() +
                tile.min_max_pairs.size() * sizeof(std::uint16_t) +
                tile.parent_fallback_heights.size() * sizeof(std::uint16_t);
            auto result = std::make_shared<std::vector<std::byte>>(total);
            lux::render::TerrainWirePageDataHeader header{
                static_cast<std::uint32_t>(tile.heights.size()),
                static_cast<std::uint32_t>(tile.weight_planes[0].size()),
                static_cast<std::uint32_t>(tile.holes.size()),
                static_cast<std::uint32_t>(tile.min_max_pairs.size() / 2u),
                static_cast<std::uint32_t>(
                    tile.parent_fallback_heights.size()),
                {0u, 0u, 0u}};
            std::size_t offset = 0u;
            const auto append = [&](const void* source, std::size_t bytes)
            {
                if (bytes != 0u)
                    std::memcpy(result->data() + offset, source, bytes);
                offset += bytes;
            };
            append(&header, sizeof(header));
            append(
                tile.heights.data(),
                tile.heights.size() * sizeof(std::uint16_t));
            append(
                tile.weight_planes[0].data(),
                tile.weight_planes[0].size());
            append(
                tile.weight_planes[1].data(),
                tile.weight_planes[1].size());
            append(tile.holes.data(), tile.holes.size());
            append(
                tile.min_max_pairs.data(),
                tile.min_max_pairs.size() * sizeof(std::uint16_t));
            append(
                tile.parent_fallback_heights.data(),
                tile.parent_fallback_heights.size() * sizeof(std::uint16_t));
            return result;
        }

        [[nodiscard]] TerrainResult prepareTerrain(
            PrepareTerrainTile request) noexcept
        {
            if (request.content.empty() || request.request_generation == 0u)
            {
                return lux::cxx::unexpected(failure(
                    ESceneGeometryPrepareError::INVALID_REQUEST,
                    "Terrain preparation request is empty"));
            }
            if (!matchesTerrain(request.reference, request.content.view()))
            {
                return lux::cxx::unexpected(failure(
                    ESceneGeometryPrepareError::CONTENT_MISMATCH,
                    "Terrain content type or schema does not match"));
            }
            auto decoded = lux::terrain::decodeTerrainTileBlob(
                request.content.view());
            if (!decoded)
            {
                return lux::cxx::unexpected(failure(
                    ESceneGeometryPrepareError::DECODE_FAILED,
                    decoded.error().detail));
            }
            PreparedTerrainTile result;
            result.request_generation = request.request_generation;
            result.height_min = decoded->height_min;
            result.height_max = decoded->height_max;
            result.sample_spacing = decoded->sample_spacing;
            result.weight_layer_count = decoded->weight_layer_count;
            result.wire = makeTerrainWireData(*decoded);
            return result;
        }

        template <class Operation>
        struct PendingPrepare final
        {
            using Result = lux::cxx::expected<
                typename Operation::Value,
                typename Operation::Error>;

            PendingPrepare(
                lux::exec::AsyncOperationCompletion<Operation> value,
                std::shared_ptr<detail::SceneGeometryPrepareReservation>
                    admission_value) noexcept
                : completion(std::move(value)),
                  admission(std::move(admission_value))
            {}

            void settle(Result result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                admission.reset();
                if (result)
                {
                    completion.complete(std::move(*result));
                    return;
                }
                completion.complete(lux::cxx::unexpected(
                    lux::exec::AsyncFailure<typename Operation::Error>::
                        domain(std::move(result.error()))));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                admission.reset();
                completion.failRuntime(
                    lux::exec::EAsyncSubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<Operation> completion;
            std::shared_ptr<detail::SceneGeometryPrepareReservation>
                admission;
        };
    } // namespace

    ClassicMeshPrepareClient::ClassicMeshPrepareClient(
        std::weak_ptr<detail::SceneGeometryPrepareControl> control,
        std::uint64_t generation,
        lux::exec::AsyncOperationClient<PrepareClassicMeshBatch>
            operation) noexcept
        : control_(std::move(control)),
          generation_(generation),
          operation_(std::move(operation))
    {}

    ClassicMeshPrepareClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        return control && control->accepts(generation_) &&
            static_cast<bool>(operation_);
    }

    lux::cxx::expected<
        ClassicMeshPrepareSender,
        lux::exec::EAsyncSubmitError>
    ClassicMeshPrepareClient::execute(
        PrepareClassicMeshBatch request) const noexcept
    {
        const auto control = control_.lock();
        if (!control || !operation_)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::UNKNOWN_OPERATION);
        }
        const auto estimated = classicMeshWorksetBytes(
            request.content.view());
        if (!estimated)
            return lux::cxx::unexpected(estimated.error());
        const auto bytes = *estimated;
        auto admission = control->reserve(
            generation_,
            detail::ESceneGeometryPrepareDomain::CLASSIC_MESH,
            bytes);
        if (!admission)
            return lux::cxx::unexpected(admission.error());
        request.admission_ = std::move(*admission);
        return lux::exec::execute(
            operation_,
            std::move(request),
            lux::exec::AsyncSubmitOptions{.accounted_bytes = bytes});
    }

    TerrainPrepareClient::TerrainPrepareClient(
        std::weak_ptr<detail::SceneGeometryPrepareControl> control,
        std::uint64_t generation,
        lux::exec::AsyncOperationClient<PrepareTerrainTile>
            operation) noexcept
        : control_(std::move(control)),
          generation_(generation),
          operation_(std::move(operation))
    {}

    TerrainPrepareClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        return control && control->accepts(generation_) &&
            static_cast<bool>(operation_);
    }

    lux::cxx::expected<TerrainPrepareSender, lux::exec::EAsyncSubmitError>
    TerrainPrepareClient::execute(PrepareTerrainTile request) const noexcept
    {
        const auto control = control_.lock();
        if (!control || !operation_)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::UNKNOWN_OPERATION);
        }
        const auto estimated = terrainWorksetBytes(request.content.view());
        if (!estimated)
            return lux::cxx::unexpected(estimated.error());
        const auto bytes = *estimated;
        auto admission = control->reserve(
            generation_,
            detail::ESceneGeometryPrepareDomain::TERRAIN,
            bytes);
        if (!admission)
            return lux::cxx::unexpected(admission.error());
        request.admission_ = std::move(*admission);
        return lux::exec::execute(
            operation_,
            std::move(request),
            lux::exec::AsyncSubmitOptions{.accounted_bytes = bytes});
    }

    lux::cxx::expected<
        SceneGeometryPrepareService,
        lux::exec::AsyncAssemblyFailure>
    SceneGeometryPrepareService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder,
        SceneGeometryPrepareConfig config)
    {
        if (!config.valid())
        {
            return lux::cxx::unexpected(lux::exec::AsyncAssemblyFailure{
                lux::exec::EAsyncAssemblyError::INVALID_QUEUE,
                lux::exec::kAsyncTypeToken<PrepareClassicMeshBatch>,
                {}});
        }
        auto control =
            std::make_shared<detail::SceneGeometryPrepareControl>(config);
        auto classic_mesh = builder.addOperation<PrepareClassicMeshBatch>(
            [control](
                PrepareClassicMeshBatch&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<
                    PrepareClassicMeshBatch>&& completion) noexcept
            {
                auto pending = std::make_shared<
                    PendingPrepare<PrepareClassicMeshBatch>>(
                        std::move(completion),
                        std::move(request.admission_));
                if (!pending->admission)
                {
                    pending->settle(lux::cxx::unexpected(failure(
                        ESceneGeometryPrepareError::INVALID_REQUEST,
                        "Classic Mesh admission is missing")));
                    return;
                }
                if (control->isClosing())
                {
                    pending->settle(lux::cxx::unexpected(failure(
                        ESceneGeometryPrepareError::SERVICE_CLOSED,
                        "geometry preparation service is closed")));
                    return;
                }
                auto work = ex::schedule(lux::exec::backgroundCpuScheduler(
                                context.runtime())) |
                    ex::then(
                        [request = std::move(request)]() mutable noexcept
                            -> ClassicResult
                        {
                            return prepareClassicMesh(std::move(request));
                        }) |
                    ex::continues_on(lux::exec::mainThreadScheduler(
                        context.runtime())) |
                    ex::then(
                        [pending](ClassicResult result) noexcept
                        {
                            pending->settle(std::move(result));
                        }) |
                    ex::upon_stopped(
                        [pending]() noexcept { pending->stop(); });
                if (!lux::exec::spawn(context.scope(), std::move(work)))
                    pending->stop();
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                config.classic_mesh.capacity,
                config.classic_mesh.byte_budget,
                config.classic_mesh.drain_batch});
        if (!classic_mesh)
            return lux::cxx::unexpected(classic_mesh.error());

        auto terrain = builder.addOperation<PrepareTerrainTile>(
            [control](
                PrepareTerrainTile&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<PrepareTerrainTile>&&
                    completion) noexcept
            {
                auto pending = std::make_shared<
                    PendingPrepare<PrepareTerrainTile>>(
                        std::move(completion),
                        std::move(request.admission_));
                if (!pending->admission)
                {
                    pending->settle(lux::cxx::unexpected(failure(
                        ESceneGeometryPrepareError::INVALID_REQUEST,
                        "Terrain admission is missing")));
                    return;
                }
                if (control->isClosing())
                {
                    pending->settle(lux::cxx::unexpected(failure(
                        ESceneGeometryPrepareError::SERVICE_CLOSED,
                        "geometry preparation service is closed")));
                    return;
                }
                auto work = ex::schedule(lux::exec::backgroundCpuScheduler(
                                context.runtime())) |
                    ex::then(
                        [request = std::move(request)]() mutable noexcept
                            -> TerrainResult
                        {
                            return prepareTerrain(std::move(request));
                        }) |
                    ex::continues_on(lux::exec::mainThreadScheduler(
                        context.runtime())) |
                    ex::then(
                        [pending](TerrainResult result) noexcept
                        {
                            pending->settle(std::move(result));
                        }) |
                    ex::upon_stopped(
                        [pending]() noexcept { pending->stop(); });
                if (!lux::exec::spawn(context.scope(), std::move(work)))
                    pending->stop();
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                config.terrain.capacity,
                config.terrain.byte_budget,
                config.terrain.drain_batch});
        if (!terrain)
            return lux::cxx::unexpected(terrain.error());
        return SceneGeometryPrepareService{
            std::move(control), std::move(*classic_mesh), std::move(*terrain)};
    }

    SceneGeometryPrepareService::SceneGeometryPrepareService(
        std::shared_ptr<detail::SceneGeometryPrepareControl> control,
        lux::exec::AsyncOperationClient<PrepareClassicMeshBatch>
            classic_mesh,
        lux::exec::AsyncOperationClient<PrepareTerrainTile> terrain) noexcept
        : control_(std::move(control)),
          classic_mesh_(std::move(classic_mesh)),
          terrain_(std::move(terrain))
    {}

    SceneGeometryPrepareService::SceneGeometryPrepareService(
        SceneGeometryPrepareService&& other) noexcept
        : control_(std::move(other.control_)),
          classic_mesh_(std::move(other.classic_mesh_)),
          terrain_(std::move(other.terrain_)),
          closed_(std::exchange(other.closed_, true))
    {}

    SceneGeometryPrepareService& SceneGeometryPrepareService::operator=(
        SceneGeometryPrepareService&& other) noexcept
    {
        if (this == &other)
            return *this;
        close();
        control_ = std::move(other.control_);
        classic_mesh_ = std::move(other.classic_mesh_);
        terrain_ = std::move(other.terrain_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    SceneGeometryPrepareService::~SceneGeometryPrepareService()
    {
        close();
    }

    ClassicMeshPrepareClient
    SceneGeometryPrepareService::classicMeshClient() const noexcept
    {
        return !closed_ && control_
            ? ClassicMeshPrepareClient{
                  control_, control_->currentGeneration(), classic_mesh_}
            : ClassicMeshPrepareClient{};
    }

    TerrainPrepareClient
    SceneGeometryPrepareService::terrainClient() const noexcept
    {
        return !closed_ && control_
            ? TerrainPrepareClient{
                  control_, control_->currentGeneration(), terrain_}
            : TerrainPrepareClient{};
    }

    SceneGeometryPrepareSnapshot
    SceneGeometryPrepareService::snapshot() const noexcept
    {
        return control_ ? control_->snapshot()
                        : SceneGeometryPrepareSnapshot{.closing = true};
    }

    void SceneGeometryPrepareService::close() noexcept
    {
        if (closed_)
            return;
        closed_ = true;
        if (control_)
            control_->closeAdmission();
        classic_mesh_ = {};
        terrain_ = {};
    }
} // namespace lux::runtime
