#pragma once
#include <vulkan/vulkan.h>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <unordered_map>

namespace lux::render
{
	class ResourceContext;

	/// Key for matching cached transient resources during pool lookups.
	/// Two resources with the same key can be reused interchangeably.
	struct TransientResourceKey
	{
		ERGResourceType     type{};
		uint32_t            width{0}, height{0}, depth{0};
		lux::common::ETextureFormat    format{};
		ERGTextureUsageFlags usage{0};
		lux::common::ETextureDimension dimension{};
		uint32_t            mip_levels{1};
		uint32_t            array_layers{1};
		uint32_t            sample{1};
		// Buffer fields
		uint64_t            buffer_size{0};
		ERGBufferUsageFlags buffer_usage{0};
		ERGMemoryUsage      memory_usage{};

		bool operator==(const TransientResourceKey&) const = default;
	};

	struct TransientResourceKeyHash
	{
		size_t operator()(const TransientResourceKey& k) const noexcept
		{
			// FNV-1a style hash combining
			size_t h = 14695981039346656037ULL;
			auto combine = [&](size_t v) { h ^= v; h *= 1099511628211ULL; };
			combine(static_cast<size_t>(k.type));
			combine(k.width); combine(k.height); combine(k.depth);
			combine(static_cast<size_t>(k.format));
			combine(k.usage);
			combine(static_cast<size_t>(k.dimension));
			combine(k.mip_levels); combine(k.array_layers); combine(k.sample);
			combine(static_cast<size_t>(k.buffer_size));
			combine(k.buffer_usage);
			combine(static_cast<size_t>(k.memory_usage));
			return h;
		}
	};

	struct CachedResource
	{
		VkImage       image      = VK_NULL_HANDLE;
		VkBuffer      buffer     = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		uint64_t      last_used_frame = 0;
	};

	class LUX_FUNCTION_PUBLIC RGVulkanResourceAllocator : public PhysicalResourceAllocator
	{
	public:
		RGVulkanResourceAllocator(ResourceContext& context);
		~RGVulkanResourceAllocator() override;

		Expected<RGPhysicalResourceTable> allocate(
			const RGGraphDescription& graph,
			const RGDependencyInfo&   /*deps*/,
			const RGCompileOptions&   /*options*/,
			VkExtent2D extent,
			uint32_t frames_in_flight = 1
		) override;
		
		// Release the allocated physical resource table
		void deallocate(
			const RGPhysicalResourceTable& table
		) override;
		
		// Release a single resource (using the VMA allocator in context_)
		void destroyImage(VkImage image, VmaAllocation allocation);
		void destroyBuffer(VkBuffer buffer, VmaAllocation allocation);

		/// Deallocate resources but return TRANSIENT/PERSISTENT ones to pool for reuse.
		/// Use this instead of deallocate() when you plan to recompile soon.
		void deallocateToPool(
			const RGPhysicalResourceTable& table,
			const RGGraphDescription& graph
		) override;

		/// Reclaim cached resources that have been idle for more than max_idle_frames.
		/// Call once per frame to prevent unbounded pool growth.
		void garbageCollect(uint64_t current_frame, uint64_t max_idle_frames = 4);

		/// Release all cached resources immediately (e.g., on shutdown or device lost)
		void releasePool();

	private:
		Expected<void> createImage(const RGResourceDescription& resource, RGPhysicalResource& outAllocation, VkExtent2D extent, uint32_t frames_in_flight);
		Expected<void> createBuffer(const RGResourceDescription& resource, RGPhysicalResource& outAllocation, uint32_t frames_in_flight);

		/// Try to acquire a cached resource from the pool. Returns true if found.
		bool tryAcquireFromPool(const TransientResourceKey& key, RGPhysicalResource& out);

		/// Return a resource to the pool for future reuse.
		void returnToPool(const TransientResourceKey& key, const RGPhysicalResource& res);

		/// Build a pool key from a resource description.
		static TransientResourceKey makeKey(const RGResourceDescription& resource);
		
		ResourceContext& context_;

		/// Pool of cached transient resources keyed by their creation parameters.
		/// Multiple resources with the same key can exist (multimap).
		std::unordered_multimap<TransientResourceKey, CachedResource, TransientResourceKeyHash>
			resource_pool_;

		/// VMA custom pool for transient image sub-allocation.
		/// Prevents per-image dedicated vkAllocateMemory calls by sub-allocating
		/// from large memory blocks.  Created lazily on first transient image.
		VmaPool transient_image_pool_{VK_NULL_HANDLE};

		uint64_t current_frame_ = 0;
	};
}
