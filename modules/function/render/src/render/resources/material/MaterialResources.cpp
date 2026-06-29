#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/pipeline/MaterialShaderKey.hpp>

namespace lux::render
{
	MaterialHandle MaterialResources::allocateGlobalHandle()
	{
		if (!free_handle_indices_.empty())
		{
			const uint32_t index = free_handle_indices_.back();
			free_handle_indices_.pop_back();
			handle_alive_[index] = 1u;
			return MaterialHandle{ index, handle_generations_[index] };
		}

		const uint32_t index = static_cast<uint32_t>(handle_generations_.size());
		handle_generations_.push_back(1u);
		handle_alive_.push_back(1u);
		return MaterialHandle{ index, 1u };
	}

	void MaterialResources::releaseGlobalHandle(MaterialHandle h) noexcept
	{
		if (!h.valid() || h.index >= handle_generations_.size())
			return;
		if (!handle_alive_[h.index] || handle_generations_[h.index] != h.gen)
			return;

		handle_alive_[h.index] = 0u;
		++handle_generations_[h.index];
		free_handle_indices_.push_back(h.index);
	}

	MaterialResources::MaterialResources()
	{
	}

	MaterialResources::~MaterialResources()
	{
		if (initialized_)
			shutdown();
	}

	bool MaterialResources::init(const InitInfo& info)
	{
		registry_       = info.registry;
		frames_in_flight_ = info.ssbo_config.slices;
		bucket_mgr_.seedFamilyBootstrapBuckets();

		// Initialize 5 family SSBOs
		unlit_ssbo_.init(info.ssbo_config);
		legacy_lit_ssbo_.init(info.ssbo_config);
		pbr_ssbo_.init(info.ssbo_config);
		stylized_ssbo_.init(info.ssbo_config);
		graph_ssbo_.init(info.ssbo_config);

		// Create per-frame descriptor sets
		auto& device = info.ssbo_config.device_context->logicalDevice();
		descriptor_sets_.resize(frames_in_flight_, VK_NULL_HANDLE);
		std::vector<VkDescriptorSetLayout> layouts(frames_in_flight_, info.set_layout);

		VkDescriptorSetAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		alloc_info.descriptorPool     = info.descriptor_pool;
		alloc_info.descriptorSetCount = frames_in_flight_;
		alloc_info.pSetLayouts        = layouts.data();

		VkResult result = vkAllocateDescriptorSets(device, &alloc_info, descriptor_sets_.data());
		if (result != VK_SUCCESS)
			return false;

		// Write initial descriptors for every per-frame set
		for (uint32_t i = 0; i < frames_in_flight_; ++i)
			writeDescriptorsOnSet(i);

		initialized_ = true;
		return true;
	}

	void MaterialResources::writeDescriptorsOnSet(uint32_t set_index) const
	{
		unlit_ssbo_.writeDataDescriptor(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::Unlit));
		legacy_lit_ssbo_.writeDataDescriptor(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::LegacyLit));
		pbr_ssbo_.writeDataDescriptor(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::PbrMetallicRoughness));
		stylized_ssbo_.writeDataDescriptor(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::Stylized));
		graph_ssbo_.writeDataDescriptor(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::Graph));
	}

	void MaterialResources::refreshAllDescriptors(uint32_t slice)
	{
		unlit_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[current_frame_],
			static_cast<uint32_t>(ELightingTechnique::Unlit), slice);
		legacy_lit_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[current_frame_],
			static_cast<uint32_t>(ELightingTechnique::LegacyLit), slice);
		pbr_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[current_frame_],
			static_cast<uint32_t>(ELightingTechnique::PbrMetallicRoughness), slice);
		stylized_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[current_frame_],
			static_cast<uint32_t>(ELightingTechnique::Stylized), slice);
		graph_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[current_frame_],
			static_cast<uint32_t>(ELightingTechnique::Graph), slice);
	}

	void MaterialResources::refreshAllDescriptorsOnSet(uint32_t set_index, uint32_t slice)
	{
		unlit_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::Unlit), slice);
		legacy_lit_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::LegacyLit), slice);
		pbr_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::PbrMetallicRoughness), slice);
		stylized_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::Stylized), slice);
		graph_ssbo_.writeDataDescriptorTight(
			descriptor_sets_[set_index],
			static_cast<uint32_t>(ELightingTechnique::Graph), slice);
	}

	// (MaterialResources::submit(rdesc::Material) — the builtin closure-material upload
	//  path — retired in W5a. Graph materials use submitGraph() below.)

	// Pack a flat GraphMaterialData blob into the GPU struct (shared by
	// submitGraph + modifyGraph). Graph materials are data-driven — no
	// rdesc::Material variant — so this is a separate path from submit().
	static void packGraphGpu(const GraphMaterialData& data, GraphFamilyGPU& gpu)
	{
		gpu.shading_model_id = static_cast<uint32_t>(EShadingModel::GRAPH);
		gpu.feature_mask     = 0u;
		gpu.tex_mask         = data.tex_mask;
		gpu.flags            = data.flags;

		const uint32_t np = std::min<uint32_t>(data.param_count, GraphMaterialData::kMaxParams);
		for (uint32_t i = 0; i < np; ++i)
			gpu.params[i] = { data.params[i][0], data.params[i][1],
			                  data.params[i][2], data.params[i][3] };
		for (uint32_t i = 0; i < GraphMaterialData::kMaxTextures; ++i)
			gpu.tex[i].bindless = data.tex_bindless[i];
	}

	Expected<MaterialHandle>
	MaterialResources::submitGraph(const GraphMaterialData& data,
	                               ShaderHandle gbuffer_shader,
	                               ShaderHandle forward_shader,
	                               uint64_t     shader_key,
	                               lux::rdesc::EAlphaMode alpha_mode,
	                               bool                   double_sided)
	{
		ds_revision_.bump();

		GraphFamilyGPU gpu{};
		packGraphGpu(data, gpu);
		// double_sided also rides the GPU flags (MATF_DOUBLE_SIDED) for any
		// shader-side two-sided handling; the load-bearing effect is the cull tier.
		if (double_sided)
			gpu.flags |= MATF_DOUBLE_SIDED;
		const SlotHandle local_slot = graph_ssbo_.add(gpu);

		const MaterialHandle ret = allocateGlobalHandle();
		// A graph material with its own baked shader gets its OWN bucket/PSO (R1);
		// without one (legacy callers / shader_key == 0) it shares the Graph family
		// bucket — the pre-R1 behavior. W3a: render-state (alpha_mode/double_sided)
		// folds into the graph bucket key so distinct render-state -> distinct PSO.
		const uint32_t bucket = (shader_key != 0)
			? bucket_mgr_.getOrCreateGraph(shader_key, gbuffer_shader, forward_shader,
			                               alpha_mode, double_sided)
			: bucket_mgr_.getOrCreate(ELightingTechnique::Graph, 0u);
		slot_records_.insert(ret, SlotRecord{
			.family         = ELightingTechnique::Graph,
			.shading_model  = EShadingModel::GRAPH,
			.local_slot     = local_slot,
			.feature_mask   = 0u,
			.variant_bucket = bucket,
			.packed_material_type = packMaterialType(EShadingModel::GRAPH),
		});
		return ret;
	}

	std::error_code
	MaterialResources::modifyGraph(MaterialHandle slot, const GraphMaterialData& data)
	{
		auto* rec = slot_records_.find(slot);
		if (!rec)
			return make_error_code(ERenderError::NotFound);
		if (rec->family != ELightingTechnique::Graph)
			return make_error_code(ERenderError::TypeMismatch);

		GraphFamilyGPU gpu{};
		packGraphGpu(data, gpu);
		// In-place data update at the same slot — no descriptor/graph change.
		const bool ok = graph_ssbo_.modify(rec->local_slot, gpu);
		return ok ? std::error_code{} : make_error_code(ERenderError::ModifyFailed);
	}

	void MaterialResources::remove(MaterialHandle slot)
	{
		auto* rec = slot_records_.find(slot);
		if (!rec) return;

		switch (rec->family)
		{
		case ELightingTechnique::Unlit:
			unlit_ssbo_.remove(rec->local_slot);
			break;
		case ELightingTechnique::LegacyLit:
			legacy_lit_ssbo_.remove(rec->local_slot);
			break;
		case ELightingTechnique::PbrMetallicRoughness:
			pbr_ssbo_.remove(rec->local_slot);
			break;
		case ELightingTechnique::Stylized:
			stylized_ssbo_.remove(rec->local_slot);
			break;
		case ELightingTechnique::Graph:
			graph_ssbo_.remove(rec->local_slot);
			break;
		default:
			break;
		}

		// Release this material's reference to its variant bucket so destroyed
		// materials no longer pin a bucket forever (the old leak that drove the
		// 128 soft cap and its silent Unlit-degrade). No-op for family/bootstrap
		// buckets (e.g. the shader_key==0 fallback). handleDestroyMaterial
		// invalidates all scene graphs right after this, so a freed+reused bucket
		// id is rebound to its new PSO before any instance references it.
		bucket_mgr_.release(rec->variant_bucket);

		slot_records_.erase(slot);
		releaseGlobalHandle(slot);
	}

	// (MaterialResources::modify(rdesc::Material) — the builtin closure-material modify
	//  path — retired in W5a. Graph materials use modifyGraph() / submitGraph().)

	void MaterialResources::uploadData(VkCommandBuffer cb, const FrameStamp& stamp)
	{
		const uint32_t frame_index = stamp.slotIndex();
		current_frame_ = frame_index;

		if (ds_revision_.needsWrite(frame_index))
		{
			refreshDescriptors(frame_index);
		}

		// Upload data for all 5 family SSBOs
		unlit_ssbo_.uploadDataSlice(cb, frame_index);
		legacy_lit_ssbo_.uploadDataSlice(cb, frame_index);
		pbr_ssbo_.uploadDataSlice(cb, frame_index);
		stylized_ssbo_.uploadDataSlice(cb, frame_index);
		graph_ssbo_.uploadDataSlice(cb, frame_index);
	}

} // namespace lux::render
