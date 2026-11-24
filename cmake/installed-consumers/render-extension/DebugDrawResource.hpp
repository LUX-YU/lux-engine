#pragma once

#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <algorithm>
#include <bit>
#include <atomic>
#include <cstring>
#include <vector>

namespace sdk_debug
{
    using namespace lux::render;

    struct Vertex final { float x, y; };
    struct Index final { std::uint32_t value; };

    struct Draw final
    {
        std::uint32_t first, count, program;
        RasterClipRect clip;
        RasterTargetImage image{};
    };

    class DebugDrawResource final
    {
    public:
        using Descriptor = DebugDrawResource;
        using Config = void;
        using PerFrameData = std::tuple<Vertex, Index, Draw>;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"example.debug_geometry"}, 0, 0, {}, "Indexed debug triangles for one run.",
            EResourceDomain::JOB, resourceFrameTypes(static_cast<PerFrameData*>(nullptr)),
            EResourceContentLifetime::PER_RUN
        };

        static constexpr std::uint64_t initial_capacity = 64;

        DebugDrawResource(ResourceCreateContext& context, ResourceInstanceId id) noexcept
            : device_(context.device), registry_(context.resources), id_(id), replicas_(context.frame_count) {}

        DebugDrawResource(const DebugDrawResource&) = delete;
        DebugDrawResource& operator=(const DebugDrawResource&) = delete;
        DebugDrawResource(DebugDrawResource&&) = delete;
        DebugDrawResource& operator=(DebugDrawResource&&) = delete;

        ~DebugDrawResource() noexcept
        {
            if (held) std::terminate();
            for (const auto& replica : replicas_)
                if (replica.buffer) retire(replica.buffer, replica.uses);
        }

        static ResourceProjection projection() noexcept
        {
            return {
                GraphBufferDesc{initial_capacity}, {GraphBufferRange{0, initial_capacity}}, EGraphUse::VERTEX
            };
        }

        static ResourceRegistration factory() noexcept
        {
            static const detail::StorageBindingOps ops{
                .prepareFrame = [](void* object, detail::ReplicaFrame frame, detail::ResourceUploader&) noexcept
                {
                    return static_cast<DebugDrawResource*>(object)->writeReplica(frame.slot);
                },
                .describeGraphImport = [](const void*, std::uint32_t i) noexcept -> RenderResult<ResourceProjection>
                {
                    if (i != 0) return lux::cxx::unexpected(RenderError{kStorageMismatch});
                    return projection();
                },
                .resolveGraphImport = [](void* object, std::uint32_t i, const VGraphResourceDesc& shape,
                                         std::span<detail::ResourceInput> frames) noexcept -> RenderResult<void>
                {
                    auto& replicas = static_cast<DebugDrawResource*>(object)->replicas_;
                    const auto* desc = std::get_if<GraphBufferDesc>(&shape);
                    const bool matches_projection = i == 0 && desc && desc->size == initial_capacity;
                    if (!matches_projection || replicas.size() != frames.size())
                        return lux::cxx::unexpected(RenderError{kStorageMismatch});
                    for (std::size_t f = 0; f < frames.size(); ++f)
                    {
                        auto& replica = replicas[f];
                        frames[f] = replica.binding();
                    }
                    return {};
                },
                .selectRun = [](void* object, detail::ReplicaFrame frame) noexcept
                {
                    auto& owner = *static_cast<DebugDrawResource*>(object);
                    auto& selected = owner.replicas_[frame.slot].geometry;
                    std::swap(selected, owner.pending_);
                    owner.pending_.points.clear();
                    owner.pending_.elements.clear();
                    owner.pending_.draws.clear();
                    owner.pending_.bindings.clear();
                },
                .readDrawList = [](const void* object, std::optional<std::uint32_t> slot) noexcept
                {
                    const auto& owner = *static_cast<const DebugDrawResource*>(object);
                    const auto& geometry = slot ? owner.replicas_[*slot].geometry : owner.pending_;
                    return RasterStreamData{geometry.draws, geometry.bindings, std::span{&geometry.vertex, 1}, {}};
                }
            };
            return makeResourceRegistration<Descriptor, DebugDrawResource, &create, &DebugDrawResource::update>(&ops);
        }

        static inline std::atomic_uint allocated_buffers{}, discarded_candidates{};
        static inline std::atomic_bool reject_next_publication{};
        bool held = false;

    private:
        // The pass declares vertex and index access to the geometry buffer, then the Scene View row.
        static constexpr RasterStreamBinding view_binding{0, RasterBufferSlice{{2}, 0, sizeof(ViewGpuParameters)}};

        struct Geometry final
        {
            std::vector<Vertex> points;
            std::vector<Index> elements;
            std::vector<RasterStreamDraw> draws;
            std::vector<RasterStreamBinding> bindings;
            RasterStreamVertex vertex{};
        };

        struct Replica final
        {
            nri::Buffer* buffer = nullptr;
            std::uint64_t capacity = initial_capacity;
            detail::SubmissionFrontiers uses;
            Geometry geometry;

            detail::ResourceInput binding() noexcept
            {
                return {.buffer = buffer, .size = capacity, .queue_uses = {nullptr, &uses}};
            }
        };

        void retire(nri::Buffer* buffer, const detail::SubmissionFrontiers& uses) noexcept
        {
            device_.deferRelease(uses, [buffer, destroy = device_.nriCore().DestroyBuffer]() noexcept { destroy(buffer); });
        }

        RenderResult<nri::Buffer*> allocate(std::uint64_t size) noexcept
        {
            nri::Buffer* buffer = nullptr;
            const nri::BufferDesc desc{.size = size, .usage = nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX};
            const auto result = device_.nriCore().CreatePlacedBuffer(device_.device(), NriHostUploadHeap, desc, buffer);
            if (result != nri::Result::SUCCESS)
                return lux::cxx::unexpected(RenderError{kDeviceFailure, {static_cast<std::uint32_t>(result)}});
            ++allocated_buffers;
            return buffer;
        }

        static RenderResult<std::unique_ptr<DebugDrawResource>>
        create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
        {
            auto owner = std::make_unique<DebugDrawResource>(context, id);
            for (auto& replica : owner->replicas_)
            {
                replica.uses = context.device.makeFrontiers();
                auto buffer = owner->allocate(replica.capacity);
                if (!buffer) return lux::cxx::unexpected(buffer.error());
                replica.buffer = *buffer;
            }
            return owner;
        }

        RenderResult<void>
        update(std::span<const Vertex> points, std::span<const Index> elements, std::span<const Draw> commands) noexcept
        {
            // Indices follow vertices in the same backing. Native firstIndex addresses 32-bit words.
            const auto first_index = points.size_bytes() / sizeof(Index);
            const bool outside_draw_abi = first_index > UINT32_MAX || elements.size() > UINT32_MAX - first_index;
            if (outside_draw_abi || commands.size() >= UINT32_MAX) return lux::cxx::unexpected(RenderError{kDeviceCapacity});
            const bool invalid_index = std::ranges::any_of(elements, [&](Index i) { return i.value >= points.size(); });
            if (elements.size() % 3 || invalid_index)
                return lux::cxx::unexpected(RenderError{kFrameContract});
            for (const auto& command : commands)
            {
                const bool invalid_range = command.first > elements.size() || command.count > elements.size() - command.first;
                if (invalid_range || command.count % 3 || command.program >= 3)
                    return lux::cxx::unexpected(RenderError{kFrameContract});
            }
            pending_.vertex = {0, {{0}, 0, RasterBufferSlice::whole}};
            pending_.draws.clear();
            pending_.bindings.assign(1, view_binding);
            for (const auto& command : commands)
            {
                RasterStreamIndexed indexed{
                    {{1}, 0, RasterBufferSlice::whole}, EIndexType::UINT32, command.count, 1,
                    static_cast<std::uint32_t>(first_index) + command.first
                };
                RasterStreamRange bindings{0, command.program == 0 ? 1u : 0u};
                if (command.program == 2)
                {
                    bindings = {static_cast<std::uint32_t>(pending_.bindings.size()), 1};
                    pending_.bindings.push_back({1, command.image});
                }
                pending_.draws.push_back({
                    .program = {.index = command.program}, .bindings = bindings,
                    .vertices = {0, 1}, .draw = indexed, .clip = command.clip
                });
            }
            pending_.points.assign(points.begin(), points.end());
            pending_.elements.assign(elements.begin(), elements.end());
            return {};
        }

        RenderResult<bool> writeReplica(std::uint32_t slot) noexcept
        {
            auto& replica = replicas_[slot];
            const auto& geometry = replica.geometry;
            const auto vertices = std::as_bytes(std::span{geometry.points});
            const auto indices = std::as_bytes(std::span{geometry.elements});
            const std::uint64_t bytes = vertices.size_bytes() + indices.size_bytes();
            if (!bytes) return true;

            auto* buffer = replica.buffer;
            const auto& core = device_.nriCore();
            const bool grow = bytes > replica.capacity;
            auto capacity = replica.capacity;
            if (grow)
            {
                const auto& memory = device_.description().memory;
                const auto limit = std::min(memory.bufferMaxSize, memory.allocationMaxSize);
                if (bytes > limit) return lux::cxx::unexpected(RenderError{kDeviceCapacity, {bytes, limit}});
                capacity = std::min(std::bit_ceil(bytes), limit);
                auto candidate = allocate(capacity);
                if (!candidate) return lux::cxx::unexpected(candidate.error());
                buffer = *candidate;
            }
            lux::cxx::scope_exit rollback([&]() noexcept
            {
                if (grow)
                {
                    core.DestroyBuffer(buffer);
                    ++discarded_candidates;
                }
            });
            auto* mapped = static_cast<std::byte*>(core.MapBuffer(*buffer, 0, bytes));
            if (!mapped) return lux::cxx::unexpected(RenderError{kDeviceFailure});
            if (!vertices.empty()) std::memcpy(mapped, vertices.data(), vertices.size_bytes());
            if (!indices.empty()) std::memcpy(mapped + vertices.size_bytes(), indices.data(), indices.size_bytes());
            core.UnmapBuffer(*buffer);

            if (grow)
            {
                detail::ResourceInput replacement{.buffer = buffer, .size = capacity, .queue_uses = {nullptr, &replica.uses}};
                // Installed SDK fault injection exercises the registry's range preflight after a real allocation.
                if (reject_next_publication.exchange(false)) replacement.offset = UINT64_MAX;
                auto published = registry_.publishGraphInput(id_, 0, slot, replacement);
                if (!published) return lux::cxx::unexpected(published.error());
                retire(std::exchange(replica.buffer, buffer), replica.uses);
                replica.capacity = capacity;
            }
            rollback.release();
            return true;
        }

        detail::DeviceSession& device_;
        ResourceRegistry& registry_;
        ResourceInstanceId id_;
        std::vector<Replica> replicas_;
        Geometry pending_;
    };
}
