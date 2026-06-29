#pragma once
#include <cstdint>

namespace lux::render
{
	// ---------------------------------------------------------------
	// Central descriptor set slot assignment.
	// To add a new descriptor set, insert an entry before COUNT and
	// define the corresponding binding enum + get_binding_set<> below.
	// ---------------------------------------------------------------
	enum class EDescriptorSetSlot : uint32_t
	{
		Scene      = 0,
		Instance   = 1,
		Texture    = 2,
		Light      = 3,
		Material   = 4,
		Particle   = 5,   ///< GPU particle SSBO (compute simulation + vertex billboard)
		Compute    = 6,   ///< GPU-driven compute cull descriptor set
		VertexPool = 7,   ///< Bindless vertex source array (R1.4 of render-refactor;
		                  ///<   reads in VERTEX_BIT + COMPUTE_BIT; one descriptorCount
		                  ///<   slot per registered IVertexSource)
		COUNT    // <-- add new sets before this
	};

	/// Total number of descriptor set slots (derived from EDescriptorSetSlot::COUNT).
	inline constexpr uint32_t kDescriptorSetCount = static_cast<uint32_t>(EDescriptorSetSlot::COUNT);

	/// Bitmask covering all descriptor set slots (e.g. 0x1F for 5 sets).
	inline constexpr uint32_t kAllSetsMask = (1u << kDescriptorSetCount) - 1u;

	// ---------------------------------------------------------------
	// Per-set binding enums (individual bindings within each set)
	// ---------------------------------------------------------------

	// SET 0
	enum class ESceneSetBindings : uint8_t
	{
		GLOBAL = 0,   ///< SceneGlobalGpuData[] SoA SSBO (time, delta_time, frame_number) — indexed by scene_index push constant
		VIEW   = 1,   ///< ViewGpuData[] SoA SSBO (view/proj matrices, cam_pos, viewport) — indexed by view_index push constant
		COUNT
	};

	// SET 1
	enum class EGeneralSetBindings : uint8_t
	{
		INSTANCES = 0,
		COUNT
	};

	// SET 2
	enum class ETextureSetBindings : uint8_t
	{
		TEXTURES = 0,        ///< sampler2D[] (bindless 2D textures)
		CUBE_TEXTURES = 1,   ///< samplerCube[] (bindless cubemaps)
		COUNT
	};

	// SET 3
	enum class ELightSetBindings : uint8_t
	{
		LIGHT_SPOT = 0,
		LIGHT_DIRECTIONAL = 1,
		LIGHT_POINT = 2,
		LIGHT_AREA = 3,
		SHADOW_SLICES = 4,        ///< Shadow slice SSBO (per-slice VP + bias)
		SHADOW_ATLAS = 5,         ///< PCF atlas: sampler2DArrayShadow (D32_SFLOAT)
		SHADOW_CONFIG = 6,        ///< Shadow configuration UBO
		SHADOW_SPOT_MAP = 7,      ///< Spot light slot -> shadow slice index (-1 = none)
		SHADOW_POINT_MAP = 8,     ///< Point light slot -> base slice index (-1 = none)
		SHADOW_ATLAS_EVSM = 9,    ///< EVSM blurred moments: sampler2DArray (RGBA16F)
		SHADOW_EVSM_CONFIG = 10,  ///< EVSM exponents + bleed reduction UBO
		COUNT
	};

	// SET 4 — Material Family SSBO bindings
	// Binding index = static_cast<uint32_t>(ELightingTechnique::xxx)
	// (See MaterialFamily.hpp for ELightingTechnique)
	// 0=Unlit 1=LegacyLit 2=PBR 3=Stylized 4=Graph
	inline constexpr uint32_t kMaterialFamilyBindingCount = 5;

	// SET 5
	enum class EParticleSetBindings : uint8_t
	{
		PARTICLES = 0,   ///< Particle SSBO (readable from COMPUTE + VERTEX)
		COUNT
	};

	// SET 6 — Compute cull pass (GPU-driven rendering)
	enum class EComputeSetBindings : uint8_t
	{
		CULL_UBO        = 0,   ///< Frustum planes UBO
		INSTANCE_DATA   = 1,   ///< Instance transform SSBO
		DRAW_COMMANDS   = 2,   ///< Indirect draw command SSBO
		VISIBLE_INDICES = 3,   ///< Draw-count / visible-index SSBO
		COUNT
	};

	// SET 7 — Bindless vertex source array (R1.4 of render-refactor).
	// One SSBO binding with descriptorCount = kVertexPoolMaxCount, marked
	// PARTIALLY_BOUND so registered pools sit at their assigned indices and
	// unbound slots are valid (read by no shader). VARIABLE_DESCRIPTOR_COUNT
	// keeps Vulkan validation happy with the variable-size array.
	enum class EVertexPoolSetBindings : uint8_t
	{
		VERTEX_POOLS = 0,   ///< SSBO[kVertexPoolMaxCount] of vertex pool buffers
		COUNT
	};

	/// Maximum number of vertex sources the bindless array can hold.
	/// 16 = 1 static segment + ~15 transient producers; bump when needed.
	inline constexpr uint32_t kVertexPoolMaxCount = 16;

	// ---------------------------------------------------------------
	// Compile-time mapping: binding enum → descriptor set index.
	// Values are derived from EDescriptorSetSlot to keep a single
	// source of truth.
	// ---------------------------------------------------------------
	template<typename T> struct get_binding_set;
	template<> struct get_binding_set<ESceneSetBindings>    { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Scene); };
	template<> struct get_binding_set<EGeneralSetBindings>  { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Instance); };
	template<> struct get_binding_set<ETextureSetBindings>  { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Texture); };
	template<> struct get_binding_set<ELightSetBindings>    { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Light); };
	template<> struct get_binding_set<EParticleSetBindings>   { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Particle); };
	template<> struct get_binding_set<EComputeSetBindings>    { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::Compute); };
	template<> struct get_binding_set<EVertexPoolSetBindings> { static constexpr uint32_t value = static_cast<uint32_t>(EDescriptorSetSlot::VertexPool); };
}
