#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <vk_mem_alloc.h>
#include <lux/engine/render/graph/vk_type_converter.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/graph/RGTextureUtils.hpp>

#include <array>

namespace lux::render
{
	namespace
	{
		[[nodiscard]] VmaMemoryUsage convertMemoryUsageToVma(ERGMemoryUsage memory_usage)
		{
			switch (memory_usage)
			{
				case ERGMemoryUsage::GPU_ONLY:
					return VMA_MEMORY_USAGE_GPU_ONLY;
				case ERGMemoryUsage::CPU_TO_GPU:
					return VMA_MEMORY_USAGE_CPU_TO_GPU;
				case ERGMemoryUsage::GPU_TO_CPU:
					return VMA_MEMORY_USAGE_GPU_TO_CPU;
				case ERGMemoryUsage::CPU_ONLY:
					return VMA_MEMORY_USAGE_CPU_ONLY;
				default:
					return VMA_MEMORY_USAGE_GPU_ONLY;
			}
		}
	} // namespace

	RGVulkanResourceAllocator::RGVulkanResourceAllocator(ResourceContext& context)
		: context_(context) {

	}

	RGVulkanResourceAllocator::~RGVulkanResourceAllocator()
	{
		releasePool();
	}

	TransientResourceKey RGVulkanResourceAllocator::makeKey(const RGResourceDescription& resource)
	{
		TransientResourceKey key{};
		key.type = resource.type;

		if (auto* tex = std::get_if<RGTextureDescription>(&resource.desc)) {
			key.width        = tex->width;
			key.height       = tex->height;
			key.depth        = tex->depth;
			key.format       = tex->format;
			key.usage        = tex->usage;
			key.dimension    = tex->dimension;
			key.mip_levels   = tex->mip_levels;
			key.array_layers = tex->array_layers;
			key.sample       = tex->sample;
		} else if (auto* buf = std::get_if<RGBufferDescription>(&resource.desc)) {
			key.buffer_size  = buf->size;
			key.buffer_usage = buf->usage;
			key.memory_usage = buf->memory_usage;
		}

		return key;
	}

	bool RGVulkanResourceAllocator::tryAcquireFromPool(
		const TransientResourceKey& key, RGPhysicalResource& out)
	{
		auto it = resource_pool_.find(key);
		if (it == resource_pool_.end()) return false;

		const CachedResource& cached = it->second;
		if (key.type == ERGResourceType::TEXTURE) {
			out.physical_handles.push_back(reinterpret_cast<uintptr_t>(cached.image));
		} else {
			out.physical_handles.push_back(reinterpret_cast<uintptr_t>(cached.buffer));
		}
		out.physical_allocations.push_back(reinterpret_cast<uintptr_t>(cached.allocation));

		resource_pool_.erase(it);
		return true;
	}

	void RGVulkanResourceAllocator::returnToPool(
		const TransientResourceKey& key, const RGPhysicalResource& res)
	{
		for (size_t k = 0; k < res.physical_handles.size(); ++k) {
			CachedResource cached{};
			cached.allocation     = reinterpret_cast<VmaAllocation>(res.physical_allocations[k]);
			cached.last_used_frame = current_frame_;

			if (res.type == ERGResourceType::TEXTURE) {
				cached.image = reinterpret_cast<VkImage>(res.physical_handles[k]);
			} else {
				cached.buffer = reinterpret_cast<VkBuffer>(res.physical_handles[k]);
			}

			resource_pool_.emplace(key, cached);
		}
	}

	Expected<RGPhysicalResourceTable> RGVulkanResourceAllocator::allocate(
		const RGGraphDescription& graph,
		const RGDependencyInfo&   /*deps*/,
		const RGCompileOptions&   /*options*/,
		VkExtent2D extent,
		uint32_t frames_in_flight)
	{
		const size_t resource_count = graph.resources.size();
		RGPhysicalResourceTable table;
		table.reserve(resource_count);

		for (size_t i = 0; i < resource_count; ++i)
		{
			const RGResourceDescription& resource = graph.resources[i];
			size_t index   = table.emplace(); 
			auto& phys     = table.at(static_cast<uint32_t>(index));
			phys.lifetime  = resource.lifetime;
			phys.index     = static_cast<uint32_t>(index);
			std::optional<std::error_code> create_err;
			switch (resource.lifetime)
			{
				case ERGResourceLifetime::EXTERNAL:
					// Feature owns the storage; RG never allocates it. The empty
					// physical_handles slot keeps logical indices aligned — getHandle()
					// returns 0 and barrier patching skips it. (M3 layer 2)
					phys.type = resource.type;
					continue;
				case ERGResourceLifetime::PING_PONG:
				{
					phys.ring_size     = resource.ring_size;
					phys.ring_phase    = resource.ring_phase;
					phys.pingpong_peer = resource.pingpong_peer;
					phys.type          = resource.type;
					if (resource.ring_phase == 0u)  // CURRENT owns the copies
					{
						std::visit([&](auto&& arg) {
							using T = std::decay_t<decltype(arg)>;
							if constexpr (std::is_same_v<T, RGTextureDescription>) {
								auto result = createImage(resource, phys, extent, frames_in_flight);
								if (!result) create_err = result.error();
							}
							else if constexpr (std::is_same_v<T, RGBufferDescription>) {
								auto result = createBuffer(resource, phys, frames_in_flight);
								if (!result) create_err = result.error();
							}
						}, resource.desc);
						if (create_err) { deallocate(table); return lux::cxx::unexpected(*create_err); }
					}
					// PREVIOUS (ring_phase>0): no allocation; resolveRingTarget redirects to peer.
					break;
				}
				case ERGResourceLifetime::TRANSIENT: [[fallthrough]];
				case ERGResourceLifetime::PERSISTENT:
				{
					std::visit([&](auto&& arg) {
						using T = std::decay_t<decltype(arg)>;
						if constexpr (std::is_same_v<T, RGTextureDescription>)
						{
							phys.type = ERGResourceType::TEXTURE;
							auto result = createImage(resource, phys, extent, frames_in_flight);
							if (!result) create_err = result.error();
						}
						else if constexpr (std::is_same_v<T, RGBufferDescription>)
						{
							phys.type = ERGResourceType::BUFFER;
							auto result = createBuffer(resource, phys, frames_in_flight);
							if (!result) create_err = result.error();
						}
						},
						resource.desc
					);
					if (create_err)
					{
						deallocate(table);
						return lux::cxx::unexpected(*create_err);
					}
					break;
				}
				case ERGResourceLifetime::IMPORTED:
				{
					std::visit([&](auto&& arg) {
						using T = std::decay_t<decltype(arg)>;
						if constexpr (std::is_same_v<T, RGTextureDescription>)
						{
							phys.type = ERGResourceType::TEXTURE;
							if (!resource.import_info)
							{
								create_err = make_error_code(ERenderError::InvalidArgument);
								return;
							}
							phys.image_getter = resource.import_info->image_getter;
							phys.update_group = resource.import_info->update_group;

							// Slotted images are injected per-frame via RGFrameContext::imported_slots.
							if (!phys.image_getter)
							{
								if (!resource.import_info->slot.has_value())
									create_err = make_error_code(ERenderError::InvalidArgument);
								return;
							}

							std::array<VkImage, 8> images{};
							const uint32_t count = phys.image_getter(
								images.data(), static_cast<uint32_t>(images.size()));
							if (count == 0u)
							{
								create_err = make_error_code(ERenderError::InvalidArgument);
								return;
							}
							phys.physical_handles.reserve(count);
							for (uint32_t ii = 0; ii < count; ++ii)
								phys.physical_handles.push_back(reinterpret_cast<uintptr_t>(images[ii]));
						}
						else if constexpr (std::is_same_v<T, RGBufferDescription>)
						{
							phys.type = ERGResourceType::BUFFER;
							if (!resource.import_buffer_info)
							{
								create_err = make_error_code(ERenderError::InvalidArgument);
								return;
							}
							phys.buffer_getter = resource.import_buffer_info->buffer_getter;
							phys.update_group = resource.import_buffer_info->update_group;
							if (!phys.buffer_getter)
							{
								create_err = make_error_code(ERenderError::InvalidArgument);
								return;
							}

							std::array<VkBuffer, 8> buffers{};
							const uint32_t count = phys.buffer_getter(
								buffers.data(), static_cast<uint32_t>(buffers.size()));
							if (count == 0u)
							{
								create_err = make_error_code(ERenderError::InvalidArgument);
								return;
							}
							phys.physical_handles.reserve(count);
							for (uint32_t ii = 0; ii < count; ++ii)
								phys.physical_handles.push_back(reinterpret_cast<uintptr_t>(buffers[ii]));
						}
						},
						resource.desc
					);
					if (create_err)
					{
						deallocate(table);
						return lux::cxx::unexpected(*create_err);
					}
				}
				break;
			}
		}
		return table;
	}

	Expected<void> RGVulkanResourceAllocator::createImage(const RGResourceDescription& resource, RGPhysicalResource& phy, VkExtent2D extent, uint32_t frames_in_flight)
	{
		TransientResourceKey key = makeKey(resource);
		// Patch key dimensions for RELATIVE_MODE so pool lookups match the resolved size
		const auto& tex_desc = std::get<RGTextureDescription>(resource.desc);
		auto resolved = resolveTextureExtent(tex_desc, extent);
		key.width  = resolved.width;
		key.height = resolved.height;
		// Remember the resolved extent so deallocateToPool can return this image
		// to the pool under the SAME key it was created with (#15).
		phy.pool_key_width  = resolved.width;
		phy.pool_key_height = resolved.height;

		// TRANSIENT resources need one physical image per FIF slot to prevent
		// cross-frame GPU data races (e.g. GBuffer written by frame N while
		// frame N-1 is still reading it).
		const uint32_t copies =
			(phy.lifetime == ERGResourceLifetime::PING_PONG)
				? (frames_in_flight > resource.ring_size ? frames_in_flight : resource.ring_size)
			: (phy.lifetime == ERGResourceLifetime::TRANSIENT) ? frames_in_flight : 1u;

		// Prepare create-infos once — all copies share the same parameters.
		VkImageCreateInfo image_info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		image_info.imageType     = convertTextureDimension(tex_desc.dimension, image_info.flags);
		image_info.extent.width  = resolved.width;
		image_info.extent.height = resolved.height;
		image_info.extent.depth  = tex_desc.depth;
		image_info.mipLevels     = tex_desc.mip_levels;
		image_info.arrayLayers   = tex_desc.array_layers;
		image_info.format        = convertTextureFormat(tex_desc.format);
		image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		image_info.samples       = static_cast<VkSampleCountFlagBits>(tex_desc.sample);
		image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		image_info.usage         = convertTextureUsage(tex_desc.usage);

		VmaAllocationCreateInfo alloc_info{};
		alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

		// Use a custom VMA pool for transient images to force sub-allocation
		// from large memory blocks, avoiding per-image dedicated vkAllocateMemory.
		if (phy.lifetime == ERGResourceLifetime::TRANSIENT)
		{
			if (transient_image_pool_ == VK_NULL_HANDLE)
			{
				uint32_t mem_type_index = 0;
				VmaAllocationCreateInfo probe{};
				probe.usage = VMA_MEMORY_USAGE_GPU_ONLY;
				VkResult find_res = vmaFindMemoryTypeIndexForImageInfo(
					context_.vmaAllocator(), &image_info, &probe, &mem_type_index);
				if (find_res == VK_SUCCESS)
				{
					VmaPoolCreateInfo pool_ci{};
					pool_ci.memoryTypeIndex = mem_type_index;
					pool_ci.flags = VMA_POOL_CREATE_IGNORE_BUFFER_IMAGE_GRANULARITY_BIT;
					pool_ci.blockSize = 256u * 1024u * 1024u; // 256 MB blocks
					pool_ci.maxBlockCount = 0; // unlimited
					vmaCreatePool(context_.vmaAllocator(), &pool_ci, &transient_image_pool_);
				}
			}
			if (transient_image_pool_ != VK_NULL_HANDLE)
				alloc_info.pool = transient_image_pool_;
		}

		for (uint32_t c = 0; c < copies; ++c)
		{
			// Try to reuse a cached resource from the pool first
			if (phy.lifetime == ERGResourceLifetime::TRANSIENT && tryAcquireFromPool(key, phy)) continue;

			VkImage       image      = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;

			VkResult result = vmaCreateImage(
				context_.vmaAllocator(),
				&image_info,
				&alloc_info,
				&image,
				&allocation,
				nullptr
			);

			if (result != VK_SUCCESS)
			{
				return lux::cxx::unexpected(make_error_code(ERenderError::GpuResourceCreationFailed));
			}

			phy.physical_handles.push_back(reinterpret_cast<uintptr_t>(image));
			phy.physical_allocations.push_back(reinterpret_cast<uintptr_t>(allocation));
		}
		return {};
	}

	Expected<void> RGVulkanResourceAllocator::createBuffer(const RGResourceDescription& resource, RGPhysicalResource& phy, uint32_t frames_in_flight)
	{
		TransientResourceKey key = makeKey(resource);
		const auto& buf_desc = std::get<RGBufferDescription>(resource.desc);

		VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		buffer_info.size        = buf_desc.size;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		buffer_info.usage       = convertBufferUsage(buf_desc.usage);

		VmaAllocationCreateInfo alloc_info{};
		alloc_info.usage = convertMemoryUsageToVma(buf_desc.memory_usage);

		const uint32_t copies =
			(phy.lifetime == ERGResourceLifetime::PING_PONG)
				? (frames_in_flight > resource.ring_size ? frames_in_flight : resource.ring_size)
			: (phy.lifetime == ERGResourceLifetime::TRANSIENT) ? frames_in_flight : 1u;

		for (uint32_t c = 0; c < copies; ++c)
		{
			if (phy.lifetime == ERGResourceLifetime::TRANSIENT && tryAcquireFromPool(key, phy)) continue;

			VkBuffer      buffer     = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;

			VkResult result = vmaCreateBuffer(
				context_.vmaAllocator(),
				&buffer_info,
				&alloc_info,
				&buffer,
				&allocation,
				nullptr
			);

			if (result != VK_SUCCESS)
			{
				return lux::cxx::unexpected(make_error_code(ERenderError::GpuResourceCreationFailed));
			}

			phy.physical_handles.push_back(reinterpret_cast<uintptr_t>(buffer));
			phy.physical_allocations.push_back(reinterpret_cast<uintptr_t>(allocation));
		}
		return {};
	}

	void RGVulkanResourceAllocator::deallocate(
		const RGPhysicalResourceTable& table)
	{
		// Direct destruction — resources are NOT returned to pool
		for (uint32_t i = 0; i < table.size(); ++i)
		{
			if (!table.contains(i)) {
				continue;
			}
			
			const auto& phys_res = table.at(i);
			
			if (phys_res.lifetime == ERGResourceLifetime::TRANSIENT || 
				phys_res.lifetime == ERGResourceLifetime::PERSISTENT)
			{
				if (phys_res.type == ERGResourceType::TEXTURE) {
					for (size_t k = 0; k < phys_res.physical_handles.size(); ++k) {
						VkImage image = reinterpret_cast<VkImage>(phys_res.physical_handles[k]);
						VmaAllocation allocation = reinterpret_cast<VmaAllocation>(phys_res.physical_allocations[k]);
						destroyImage(image, allocation);
					}
				} else if (phys_res.type == ERGResourceType::BUFFER) {
					for (size_t k = 0; k < phys_res.physical_handles.size(); ++k) {
						VkBuffer buffer = reinterpret_cast<VkBuffer>(phys_res.physical_handles[k]);
						VmaAllocation allocation = reinterpret_cast<VmaAllocation>(phys_res.physical_allocations[k]);
						destroyBuffer(buffer, allocation);
					}
				}
			}
		}
	}

	void RGVulkanResourceAllocator::deallocateToPool(
		const RGPhysicalResourceTable& table,
		const RGGraphDescription& graph)
	{
		// Return TRANSIENT/PERSISTENT resources to pool for future reuse
		for (uint32_t i = 0; i < table.size(); ++i)
		{
			if (!table.contains(i)) continue;
			
			const auto& phys_res = table.at(i);
			
			if (phys_res.lifetime == ERGResourceLifetime::TRANSIENT ||
				phys_res.lifetime == ERGResourceLifetime::PERSISTENT)
			{
				if (i < graph.resources.size()) {
					TransientResourceKey key = makeKey(graph.resources[i]);
					// Mirror createImage's RELATIVE_MODE patch: makeKey reads the
					// description's width/height (1/1 for RELATIVE), so without this
					// the image returns to the pool under key{1,1} and never matches
					// the resolved-extent lookup on the next allocate. (#15)
					if (phys_res.type == ERGResourceType::TEXTURE && phys_res.pool_key_width != 0) {
						key.width  = phys_res.pool_key_width;
						key.height = phys_res.pool_key_height;
					}
					returnToPool(key, phys_res);
				} else {
					// Fallback: resource index out of range, destroy directly
					if (phys_res.type == ERGResourceType::TEXTURE) {
						for (size_t k = 0; k < phys_res.physical_handles.size(); ++k) {
							destroyImage(
								reinterpret_cast<VkImage>(phys_res.physical_handles[k]),
								reinterpret_cast<VmaAllocation>(phys_res.physical_allocations[k]));
						}
					} else {
						for (size_t k = 0; k < phys_res.physical_handles.size(); ++k) {
							destroyBuffer(
								reinterpret_cast<VkBuffer>(phys_res.physical_handles[k]),
								reinterpret_cast<VmaAllocation>(phys_res.physical_allocations[k]));
						}
					}
				}
			}
		}
	}

	void RGVulkanResourceAllocator::garbageCollect(uint64_t current_frame, uint64_t max_idle_frames)
	{
		current_frame_ = current_frame;
		for (auto it = resource_pool_.begin(); it != resource_pool_.end(); )
		{
			if (current_frame - it->second.last_used_frame > max_idle_frames)
			{
				// Resource has been idle too long — destroy it
				const CachedResource& cached = it->second;
				if (cached.image != VK_NULL_HANDLE) {
					destroyImage(cached.image, cached.allocation);
				} else if (cached.buffer != VK_NULL_HANDLE) {
					destroyBuffer(cached.buffer, cached.allocation);
				}
				it = resource_pool_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void RGVulkanResourceAllocator::releasePool()
	{
		for (auto& [key, cached] : resource_pool_)
		{
			if (cached.image != VK_NULL_HANDLE) {
				destroyImage(cached.image, cached.allocation);
			} else if (cached.buffer != VK_NULL_HANDLE) {
				destroyBuffer(cached.buffer, cached.allocation);
			}
		}
		resource_pool_.clear();

		if (transient_image_pool_ != VK_NULL_HANDLE)
		{
			vmaDestroyPool(context_.vmaAllocator(), transient_image_pool_);
			transient_image_pool_ = VK_NULL_HANDLE;
		}
	}

	void RGVulkanResourceAllocator::destroyImage(VkImage image, VmaAllocation allocation)
	{
		if (image != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE)
		{
			vmaDestroyImage(context_.vmaAllocator(), image, allocation);
		}
	}

	void RGVulkanResourceAllocator::destroyBuffer(VkBuffer buffer, VmaAllocation allocation)
	{
		if (buffer != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(context_.vmaAllocator(), buffer, allocation);
		}
	}
}
