#pragma once
/**
 * @file MeshInputPool.hpp
 * @brief Per-scene wrapper exposing the global static VBO as a bindless pool.
 *
 * Every drawable — static or skinned — reads vertices through the
 * set-7 bindless vertex-pool array. This resource
 * registers the MeshResources global VBO (segment 0) with the scene's
 * VertexPoolRegistry so the _vp mesh shaders fetch via prop.vertex_pool_id
 * and prop.vertex_base.
 *
 * Lazy registration: the global VBO does not exist at scene-create time;
 * ensureRegistered() registers (and later refreshes on VBO realloc) once
 * MeshResources has uploaded data. Pure-static scenes (no SkinningFeature)
 * also drive this resource — it is created unconditionally in
 * RenderScene's constructor.
 *
 * Lifetime: per-scene. The destructor unregisters from VertexPoolRegistry.
 * SkinningResources's compute kernel consumes the same pool id as its
 * INPUT (replacing its old private StaticVertexSource ownership).
 */

#include <lux/engine/render/resources/vertex/StaticVertexSource.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/core/VertexLayoutTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC MeshInputPool
    {
    public:
        MeshInputPool() = default;
        ~MeshInputPool() { shutdown(); }

        MeshInputPool(const MeshInputPool&)            = delete;
        MeshInputPool& operator=(const MeshInputPool&) = delete;

        struct InitInfo
        {
            VertexPoolRegistry* vertex_pool_registry{nullptr};
            MeshResources*      mesh_resources{nullptr};
            VertexLayoutId      layout_id{kDefaultVertexLayoutId};
            std::uint16_t       vbo_segment{0};
        };

        bool init(const InitInfo& info)
        {
            if (initialized_) return true;
            if (!info.vertex_pool_registry || !info.mesh_resources) return false;
            vpr_     = info.vertex_pool_registry;
            source_  = std::make_unique<StaticVertexSource>(
                *info.mesh_resources, info.vbo_segment, info.layout_id);
            initialized_ = true;
            return true;
        }

        void shutdown() noexcept
        {
            if (!initialized_) return;
            if (pool_id_ != ~0u && vpr_)
            {
                vpr_->unregisterSource(pool_id_);
                pool_id_ = ~0u;
            }
            source_.reset();
            vpr_               = nullptr;
            registered_buffer_ = VK_NULL_HANDLE;
            initialized_       = false;
        }

        /// Claim a bindless pool slot once the global VBO exists, and refresh
        /// the descriptor if the VBO reallocated (grew). Idempotent. Returns
        /// the pool id (~0u while the VBO is still absent).
        std::uint32_t ensureRegistered() noexcept
        {
            if (!source_ || !vpr_) return ~0u;
            const VkBuffer cur = source_->buffer();
            if (cur == VK_NULL_HANDLE) return ~0u;
            if (pool_id_ == ~0u)
            {
                pool_id_           = vpr_->registerSource(*source_);
                registered_buffer_ = cur;
            }
            else if (cur != registered_buffer_)
            {
                vpr_->refreshSource(pool_id_);
                registered_buffer_ = cur;
            }
            return pool_id_;
        }

        [[nodiscard]] std::uint32_t       poolId()      const noexcept { return pool_id_; }
        [[nodiscard]] StaticVertexSource* source()            noexcept { return source_.get(); }
        [[nodiscard]] bool                initialized() const noexcept { return initialized_; }

    private:
        std::unique_ptr<StaticVertexSource> source_;
        VertexPoolRegistry*                 vpr_{nullptr};
        std::uint32_t                       pool_id_{~0u};
        VkBuffer                            registered_buffer_{VK_NULL_HANDLE};
        bool                                initialized_{false};
    };
} // namespace lux::render
