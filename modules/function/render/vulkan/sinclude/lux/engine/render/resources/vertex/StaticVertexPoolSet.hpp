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
        StaticVertexPoolSet() = default;
        ~StaticVertexPoolSet() { shutdown(); }

        StaticVertexPoolSet(const StaticVertexPoolSet&) = delete;
        StaticVertexPoolSet& operator=(const StaticVertexPoolSet&) = delete;

        struct InitInfo final
        {
            VertexPoolRegistry* vertex_pool_registry{nullptr};
            MeshResources* mesh_resources{nullptr};
        };

        [[nodiscard]] bool init(const InitInfo& info)
        {
            if (initialized_)
                return true;
            if (info.vertex_pool_registry == nullptr ||
                info.mesh_resources == nullptr)
            {
                return false;
            }
            vertex_pool_registry_ = info.vertex_pool_registry;
            mesh_resources_ = info.mesh_resources;
            initialized_ = true;
            return true;
        }

        void shutdown() noexcept
        {
            if (!initialized_)
                return;
            if (vertex_pool_registry_ != nullptr)
            {
                for (auto& [key, entry] : entries_)
                {
                    (void)key;
                    if (entry.pool_id != ~0u)
                        vertex_pool_registry_->unregisterSource(entry.pool_id);
                }
            }
            entries_.clear();
            mesh_resources_ = nullptr;
            vertex_pool_registry_ = nullptr;
            initialized_ = false;
        }

        [[nodiscard]] std::uint32_t ensureRegistered(
            std::uint16_t vbo_segment,
            VertexLayoutId layout_id) noexcept
        {
            if (!initialized_ ||
                vbo_segment >= mesh_resources_->vboSegmentCount() ||
                layout_id == kInvalidVertexLayoutId)
            {
                return ~0u;
            }

            const Key key{vbo_segment, layout_id};
            auto [iterator, inserted] = entries_.try_emplace(key);
            auto& entry = iterator->second;
            if (inserted)
            {
                entry.source = std::make_unique<StaticVertexSource>(
                    *mesh_resources_,
                    vbo_segment,
                    layout_id);
            }

            const VkBuffer current = entry.source->buffer();
            if (current == VK_NULL_HANDLE)
                return ~0u;
            if (entry.pool_id == ~0u)
            {
                entry.pool_id = vertex_pool_registry_->registerSource(
                    *entry.source);
                entry.registered_buffer = current;
            }
            else if (current != entry.registered_buffer)
            {
                vertex_pool_registry_->refreshSource(entry.pool_id);
                entry.registered_buffer = current;
            }
            return entry.pool_id;
        }

        [[nodiscard]] VertexSourceHandle handleForMesh(
            MeshHandle mesh) noexcept
        {
            if (!initialized_)
                return kInvalidVertexSourceHandle;
            const auto* record = mesh_resources_->getGpuRecord(mesh);
            if (record == nullptr ||
                ensureRegistered(record->vbo_segment, record->layout_id) == ~0u)
            {
                return kInvalidVertexSourceHandle;
            }
            const auto iterator = entries_.find(
                Key{record->vbo_segment, record->layout_id});
            return iterator == entries_.end()
                ? kInvalidVertexSourceHandle
                : iterator->second.source->handleForMesh(mesh);
        }

        [[nodiscard]] bool initialized() const noexcept
        {
            return initialized_;
        }

        [[nodiscard]] std::uint32_t registeredPoolCount() const noexcept
        {
            std::uint32_t result = 0u;
            for (const auto& [key, entry] : entries_)
            {
                (void)key;
                result += entry.pool_id != ~0u ? 1u : 0u;
            }
            return result;
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
            std::uint32_t pool_id{~0u};
            VkBuffer registered_buffer{VK_NULL_HANDLE};
        };

        std::unordered_map<Key, Entry, KeyHash> entries_;
        VertexPoolRegistry* vertex_pool_registry_{nullptr};
        MeshResources* mesh_resources_{nullptr};
        bool initialized_{false};
    };
} // namespace lux::render
