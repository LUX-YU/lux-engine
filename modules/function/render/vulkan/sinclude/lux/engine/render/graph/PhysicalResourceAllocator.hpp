#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <limits>
#include <algorithm>
#include <utility>

#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/function/render/graph/DependencyAnalyzer.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/cxx/compile_time/type_info.hpp>

#include <lux/cxx/container/SparseSet.hpp>

namespace lux::render
{
    struct RGCompileOptions;
    struct RGPhysicalResource
    {
        uint32_t            index;
        ERGResourceType     type;                 // Resource type (BUFFER or TEXTURE)
		ERGResourceLifetime lifetime;             // Resource lifetime type
        
        // Support multi-frame resources.
        // For normal resources, vector size is 1.
        // For Swapchain/PerFrame resources, size is frames_in_flight.
        std::vector<uintptr_t> physical_handles; 
        std::vector<uintptr_t> physical_allocations;

        // Callback for dynamically fetching external resource handles
        ExternalImageGetter image_getter = nullptr;

        // Callback for dynamically fetching external buffer handles
        ExternalBufferGetter buffer_getter = nullptr;

        // Update group mask for targeted resource refresh filtering
        uint32_t update_group = 0;

        // Pool key dimensions actually used when this texture was created. For
        // RELATIVE_MODE textures the description carries width=height=1 (the real
        // size is screen_extent * scale, resolved at create time), so the dealloc
        // pool key must be patched with THESE resolved dimensions — otherwise the
        // image is returned to the pool under key{1,1} and never matches the
        // resolved-extent lookup of a future allocation (pool reuse defeated for
        // every screen-relative target). 0 = not a sized texture. (#15)
        uint32_t pool_key_width  = 0;
        uint32_t pool_key_height = 0;

        // PING_PONG ring info (mirrors RGResourceDescription; filled by allocate()).
        // The CURRENT resource (ring_phase 0) owns physical_handles; a PREVIOUS/
        // history resource (ring_phase>0) has empty handles and resolves through
        // pingpong_peer — see resolveRingTarget().
        uint32_t ring_size     = 1;
        uint32_t ring_phase    = 0;
        int32_t  pingpong_peer = -1;

        // Helper function: Get handle for current frame
        uintptr_t getHandle(uint32_t frame_index) const {
            if (physical_handles.empty()) return 0;
            // If resource is not per-frame, always return the 0th one
            if (physical_handles.size() == 1) return physical_handles[0];
            return physical_handles[frame_index % physical_handles.size()];
        }
    };

    using RGPhysicalResourceTable = lux::cxx::OffsetAutoSparseSet<uint32_t, RGPhysicalResource>;

    /// Resolve a (possibly ping-pong) logical resource to the physical resource
    /// index that OWNS the handles plus the copy/frame index to read. Identity for
    /// a normal resource; for a ping-pong PREVIOUS/history phase it redirects to the
    /// peer (the writer) and rewinds the frame index by ring_phase. The result is
    /// fed to per_frame_views[idx][frame] / getHandle(frame) at every resolve site.
    /// MVP: copies == ring_size == FIF (HZB), so slot parity == absolute-frame parity.
    inline std::pair<uint32_t, uint32_t> resolveRingTarget(
        const RGPhysicalResourceTable& table, uint32_t res_idx, uint32_t frame_index)
    {
        if (const auto* pr = table.tryGet(res_idx))
        {
            if (pr->lifetime == ERGResourceLifetime::PING_PONG
                && pr->ring_phase > 0 && pr->pingpong_peer >= 0)
            {
                const auto peer_idx = static_cast<uint32_t>(pr->pingpong_peer);
                if (const auto* peer = table.tryGet(peer_idx))
                {
                    const uint32_t copies = static_cast<uint32_t>(peer->physical_handles.size());
                    if (copies > 0)
                        return { peer_idx, (frame_index + copies - pr->ring_phase) % copies };
                }
            }
        }
        return { res_idx, frame_index };
    }

    class LUX_FUNCTION_PUBLIC PhysicalResourceAllocator
    {
    public:
        PhysicalResourceAllocator() = default;
        virtual ~PhysicalResourceAllocator() = default;

        // Specifically for updating handles of Imported resources (e.g., Swapchain)
        void updateImportedResource(RGPhysicalResourceTable& table, uint32_t logic_index, void* new_handle, uint32_t frame_index = 0)
        {
            // Find physical resource corresponding to logical resource
            if (table.contains(logic_index)) {
                auto& res = table[logic_index];
                if (res.physical_handles.empty()) {
                    res.physical_handles.push_back(reinterpret_cast<uintptr_t>(new_handle));
                } else if (res.physical_handles.size() == 1) {
                    res.physical_handles[0] = reinterpret_cast<uintptr_t>(new_handle);
                } else {
                    res.physical_handles[frame_index % res.physical_handles.size()] = reinterpret_cast<uintptr_t>(new_handle);
                }
            }
        }

        // Update all frame handles at once (used for Swapchain resize)
        void setImportedMultipart(RGPhysicalResourceTable& table, uint32_t logic_index, const std::vector<uintptr_t>& handles)
        {
            if (table.contains(logic_index)) {
                auto& res = table[logic_index];
                res.physical_handles = handles;
            }
        }

        // Refresh external resources with targeted filtering
        // @param table Physical resource table
        // @param indices Pre-computed dynamic resource indices from RGCompiledGraph
        // @param target_group_mask Update group mask (e.g., GROUP_SWAPCHAIN)
        void refreshExternalResources(
            RGPhysicalResourceTable& table,
            const std::vector<uint32_t>& indices,
            uint32_t target_group_mask)
        {
            constexpr uint32_t kGetterScratchCapacity = 8u;
            // Only iterate pre-marked dynamic resource indices (usually 1-3)
            for (uint32_t index : indices)
            {
                // Safety check: ensure index is valid
                if (!table.contains(index)) continue;

                // Get mutable reference
                RGPhysicalResource& res = table.at(index);

                // 1. Check mask: only refresh matching groups
                if ((res.update_group & target_group_mask) == 0) {
                    continue; // Does not belong to the requested group, skip
                }

                // 2. Check and invoke callback
                if (res.image_getter)
                {
                    std::array<VkImage, kGetterScratchCapacity> new_images{};
                    const uint32_t count = res.image_getter(new_images.data(),
                                                            static_cast<uint32_t>(new_images.size()));
                    // Only update when valid handles are obtained (robustness)
                    if (count > 0)
                    {
                        res.physical_handles.clear();
                        res.physical_handles.reserve(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            const auto img = new_images[i];
                            res.physical_handles.push_back(reinterpret_cast<uintptr_t>(img));
                        }
                    }
                }
                else if (res.buffer_getter)
                {
                    std::array<VkBuffer, kGetterScratchCapacity> new_buffers{};
                    const uint32_t count = res.buffer_getter(new_buffers.data(),
                                                             static_cast<uint32_t>(new_buffers.size()));
                    if (count > 0)
                    {
                        res.physical_handles.clear();
                        res.physical_handles.reserve(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            const auto buf = new_buffers[i];
                            res.physical_handles.push_back(reinterpret_cast<uintptr_t>(buf));
                        }
                    }
                }
            }
        }

        virtual Expected<RGPhysicalResourceTable> allocate(
            const RGGraphDescription& graph,
            const RGDependencyInfo& deps,
            const RGCompileOptions& options,
            VkExtent2D extent,
            uint32_t frames_in_flight = 1
        ) = 0;
        
        // Release allocated physical resource table
        virtual void deallocate(
            const RGPhysicalResourceTable& table
        ) = 0;

        /// Release the table, returning transient/persistent resources to the reuse
        /// pool when the concrete allocator supports pooling. Default: plain
        /// deallocate() (no pool reuse). @p graph is the description the table was
        /// allocated from — pool-key context a pooling allocator needs; the default
        /// ignores it. Lets callers (e.g. graph resize) request pool-reuse cleanup
        /// polymorphically, without a runtime type check + down-cast.
        virtual void deallocateToPool(
            const RGPhysicalResourceTable& table,
            const RGGraphDescription& /*graph*/)
        {
            deallocate(table);
        }
    };
}
