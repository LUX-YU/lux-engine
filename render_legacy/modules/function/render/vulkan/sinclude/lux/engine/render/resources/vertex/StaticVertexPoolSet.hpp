#pragma once
/**
 * @file StaticVertexPoolSet.hpp
 * @brief Per-scene bindless registrations for stable classic-mesh VBO segments.
 *
 * MeshResources owns independently allocated VBO segments. This set lazily
 * publishes one StaticVertexSource per (segment, vertex-layout) pair, so adding
 * a segment never rewrites an existing pool descriptor or changes an existing
 * VertexSourceHandle.
 */

#include <lux/engine/function/render/features/resources/ResourceHandles.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/vertex/StaticVertexSource.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <vulkan/vulkan.h>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC StaticVertexPoolSet final
    {
    public:
        /// Both dependencies outlive this cache. Destruction occurs after scene GPU uses retire.
        StaticVertexPoolSet(VertexPoolRegistry& vertex_pool_registry, MeshResources& mesh_resources) noexcept
            : vertex_pool_registry_(vertex_pool_registry), mesh_resources_(mesh_resources)
        {
        }

        ~StaticVertexPoolSet() = default;

        StaticVertexPoolSet(const StaticVertexPoolSet&) = delete;
        StaticVertexPoolSet& operator=(const StaticVertexPoolSet&) = delete;
        StaticVertexPoolSet(StaticVertexPoolSet&&) = delete;
        StaticVertexPoolSet& operator=(StaticVertexPoolSet&&) = delete;

        [[nodiscard]] std::uint32_t ensureRegistered(std::uint16_t vbo_segment, VertexLayoutId layout_id) noexcept
        {
            const bool is_invalid_segment = vbo_segment >= mesh_resources_.vboSegmentCount();
            const bool is_invalid_layout = layout_id == kInvalidVertexLayoutId;
            const bool is_invalid_source = is_invalid_segment || is_invalid_layout;
            if (is_invalid_source)
            {
                return ~0u;
            }

            const Key key{vbo_segment, layout_id};
            auto [iterator, inserted] = entries_.try_emplace(key);
            auto& entry = iterator->second;
            if (inserted)
            {
                entry.source = std::make_unique<StaticVertexSource>(mesh_resources_, vbo_segment, layout_id);
            }

            const VkBuffer current = entry.source->buffer();
            if (current == VK_NULL_HANDLE)
                return ~0u;
            if (!entry.registration)
            {
                auto registration = vertex_pool_registry_.registerSource(*entry.source);
                if (!registration)
                {
                    return ~0u;
                }
                entry.registration = std::move(*registration);
                entry.registered_buffer = current;
            }
            else if (current != entry.registered_buffer)
            {
                entry.registration.refresh();
                entry.registered_buffer = current;
            }
            return entry.registration.poolId();
        }

        [[nodiscard]] VertexSourceHandle handleForMesh(MeshHandle mesh) noexcept
        {
            const auto* record = mesh_resources_.getGpuRecord(mesh);
            if (record == nullptr || ensureRegistered(record->vbo_segment, record->layout_id) == ~0u)
            {
                return kInvalidVertexSourceHandle;
            }
            const auto iterator = entries_.find(Key{record->vbo_segment, record->layout_id});
            return iterator == entries_.end() ? kInvalidVertexSourceHandle
                                              : iterator->second.source->handleForMesh(mesh);
        }

    private:
        struct Key final
        {
            std::uint16_t segment{0u};
            VertexLayoutId layout{kInvalidVertexLayoutId};
            [[nodiscard]] bool operator==(const Key&) const noexcept = default;
        };

        struct KeyHash final
        {
            [[nodiscard]] std::size_t operator()(const Key& key) const noexcept
            {
                const auto upper = static_cast<std::uint64_t>(key.layout) << 16u;
                return std::hash<std::uint64_t>{}(upper | key.segment);
            }
        };

        struct Entry final
        {
            std::unique_ptr<StaticVertexSource> source;
            VertexSourceRegistration registration;
            VkBuffer registered_buffer{VK_NULL_HANDLE};
        };

        std::unordered_map<Key, Entry, KeyHash> entries_;
        VertexPoolRegistry& vertex_pool_registry_;
        MeshResources& mesh_resources_;
    };
} // namespace lux::render
