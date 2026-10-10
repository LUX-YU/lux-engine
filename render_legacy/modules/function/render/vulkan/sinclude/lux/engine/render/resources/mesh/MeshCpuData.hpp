#pragma once
/**
 * MeshCpuData.hpp — CPU-only mesh metadata.
 *
 * Deliberately has ZERO Vulkan / GPU dependencies so it can be included from
 * any translation unit — including headless game code, physics, and picking
 * systems that must not pull in vulkan.h / vk_mem_alloc.h.
 *
 * MeshResources.hpp includes this header and adds the GPU-side MeshGpuRecord
 * plus the full MeshResources class (which does carry Vulkan dependencies).
 */
#include <lux/engine/math/AABB.hpp>
#include <lux/engine/math/MeshBVH.hpp>
#include <memory>

namespace lux::render
{
    /**
     * @brief CPU-side mesh metadata — available immediately after asset load.
     *
     * Contains only plain-data (no Vulkan objects), so any thread may safely
     * read it after the slot has been published.
     *
     * Fields:
     *   - `local_bounds`  Local-space AABB for broadphase culling and picking.
     *   - `bvh`           Triangle BVH for precise ray-mesh picking (optional;
     *                     null when not built or not yet complete).
     *   - `valid`         True once create() has populated the record.
     */
    struct MeshCpuRecord
    {
        math::AABB local_bounds{};          ///< Local-space AABB; valid after create()
        std::unique_ptr<math::MeshBVH> bvh; ///< Triangle BVH (optional, CPU-only)
        bool valid{false};

        MeshCpuRecord() = default;
        MeshCpuRecord(MeshCpuRecord&&) noexcept = default;
        MeshCpuRecord& operator=(MeshCpuRecord&&) noexcept = default;
        MeshCpuRecord(const MeshCpuRecord&) = delete;
        MeshCpuRecord& operator=(const MeshCpuRecord&) = delete;
    };

} // namespace lux::render
