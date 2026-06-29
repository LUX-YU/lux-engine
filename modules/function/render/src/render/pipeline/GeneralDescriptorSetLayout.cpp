#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <stdexcept>

namespace lux::render
{
    GeneralDescriptorSetLayout::~GeneralDescriptorSetLayout()
    {
        auto& device = device_context_.logicalDevice();
        for (auto& layout : layouts_)
        {
            if (layout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device, layout, nullptr);
                layout = VK_NULL_HANDLE;
            }
        }
    }

    GeneralDescriptorSetLayout& GeneralDescriptorSetLayout::operator=(GeneralDescriptorSetLayout&& other) noexcept
    {
        if (this != &other)
        {
            // Destroy our own layouts first
            auto& device = device_context_.logicalDevice();
            for (auto& layout : layouts_)
            {
                if (layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(device, layout, nullptr);
                    layout = VK_NULL_HANDLE;
                }
            }
            // Take ownership from other
            layouts_ = other.layouts_;
            other.layouts_.fill(VK_NULL_HANDLE);
        }
        return *this;
    }

    bool GeneralDescriptorSetLayout::init()
    {
        auto& device = device_context_.logicalDevice();
        constexpr uint32_t SCENE_IDX    = static_cast<uint32_t>(EDescriptorSetSlot::Scene);
        constexpr uint32_t INSTANCE_IDX = static_cast<uint32_t>(EDescriptorSetSlot::Instance);
        constexpr uint32_t TEXTURE_IDX  = static_cast<uint32_t>(EDescriptorSetSlot::Texture);
        constexpr uint32_t LIGHT_IDX    = static_cast<uint32_t>(EDescriptorSetSlot::Light);
        constexpr uint32_t MATERIAL_IDX = static_cast<uint32_t>(EDescriptorSetSlot::Material);
        constexpr uint32_t PARTICLE_IDX     = static_cast<uint32_t>(EDescriptorSetSlot::Particle);
        constexpr uint32_t COMPUTE_IDX      = static_cast<uint32_t>(EDescriptorSetSlot::Compute);
        constexpr uint32_t VERTEX_POOL_IDX  = static_cast<uint32_t>(EDescriptorSetSlot::VertexPool);

        // SET 0: Scene (2 SSBOs: SceneGlobalGpuData[] SoA + ViewGpuData[] SoA)
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

            // Binding 0: SceneGlobalGpuData[] SoA (indexed by scene_index push constant)
            bindings[0].binding         = static_cast<uint32_t>(ESceneSetBindings::GLOBAL);
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                        | VK_SHADER_STAGE_COMPUTE_BIT;

            // Binding 1: ViewGpuData[] SoA (indexed by view_index push constant)
            bindings[1].binding         = static_cast<uint32_t>(ESceneSetBindings::VIEW);
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                        | VK_SHADER_STAGE_COMPUTE_BIT;

            // UPDATE_AFTER_BIND: the view buffer can grow (reallocate) mid-flight;
            // the descriptor is re-written per FIF but older frames may still be pending.
            std::array<VkDescriptorBindingFlags, 2> bind_flags{
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bf.bindingCount  = static_cast<uint32_t>(bind_flags.size());
            bf.pBindingFlags = bind_flags.data();

            VkDescriptorSetLayoutCreateInfo create_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            create_info.bindingCount = static_cast<uint32_t>(bindings.size());
            create_info.pBindings    = bindings.data();
            create_info.pNext        = &bf;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[SCENE_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 1: General/Instances (SoA: Transform + Properties SSBO)
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings(2);
            
            // Binding 0: Instance Transforms (mat4[])
            bindings[0].binding = static_cast<uint32_t>(EGeneralSetBindings::INSTANCES);
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            
            // Binding 1: Instance Properties (flags, layer_mask, etc.)
            bindings[1].binding = static_cast<uint32_t>(EGeneralSetBindings::INSTANCES) + 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            // Per-frame descriptor sets are updated during the first N frames
            // after buffer (re)allocation.  Mark bindings UPDATE_AFTER_BIND so
            // the validation layer doesn't flag per-frame-set writes while an
            // older frame's command buffer is still pending.
            std::vector<VkDescriptorBindingFlags> inst_bind_flags(
                bindings.size(), VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);

            VkDescriptorSetLayoutBindingFlagsCreateInfo inst_bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            inst_bf.bindingCount  = static_cast<uint32_t>(inst_bind_flags.size());
            inst_bf.pBindingFlags = inst_bind_flags.data();

            VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            create_info.bindingCount = static_cast<uint32_t>(bindings.size());
            create_info.pBindings    = bindings.data();
            create_info.pNext        = &inst_bf;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[INSTANCE_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 2: Textures (Bindless Combined Image Sampler)
        //   binding 0 — sampler2D[]  (2D textures, variable count)
        //   binding 1 — samplerCube[] (cubemap textures, fixed small count)
        {
            // Query device limits so the layout can grow up to the hardware maximum.
            const auto& idx_props = device_context_.physicalDevice().descriptorIndexingProperties();
            const uint32_t raw_budget = std::min(
                idx_props.maxDescriptorSetUpdateAfterBindSampledImages,
                idx_props.maxPerStageDescriptorUpdateAfterBindSampledImages);
            // Reserve headroom for COMBINED_IMAGE_SAMPLER bindings in other
            // sets of the same pipeline layout (e.g. SHADOW_ATLAS in Light set).
            constexpr uint32_t kNonTextureReserve = 8;
            const uint32_t budget = (raw_budget > kNonTextureReserve)
                                      ? (raw_budget - kNonTextureReserve) : raw_budget;
            constexpr uint32_t kCubeBudget = 256;
            const uint32_t kCubeMaxCount = std::min(kCubeBudget, budget);
            const uint32_t k2DMaxCount   = (budget > kCubeMaxCount) ? (budget - kCubeMaxCount) : 1u;

            std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

            // binding 0: sampler2D[]
            bindings[0].binding         = static_cast<uint32_t>(ETextureSetBindings::TEXTURES);
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = k2DMaxCount;
            bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

            // binding 1: samplerCube[]
            bindings[1].binding         = static_cast<uint32_t>(ETextureSetBindings::CUBE_TEXTURES);
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = kCubeMaxCount;
            bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Per-binding flags (VARIABLE_DESCRIPTOR_COUNT on last binding only)
            std::array<VkDescriptorBindingFlags, 2> bindFlags = {
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                // last binding gets VARIABLE_DESCRIPTOR_COUNT
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
            };

            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            bf.bindingCount  = static_cast<uint32_t>(bindFlags.size());
            bf.pBindingFlags = bindFlags.data();

            VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            create_info.bindingCount = static_cast<uint32_t>(bindings.size());
            create_info.pBindings    = bindings.data();
            create_info.pNext        = &bf;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[TEXTURE_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 3: Lights (SSBO) + Shadow resources
        {
            std::vector<VkDescriptorSetLayoutBinding> light_bindings;
            
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::LIGHT_SPOT),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT
            });
            
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::LIGHT_DIRECTIONAL),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT
            });
            
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::LIGHT_POINT),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT
            });
            
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::LIGHT_AREA),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT
            });

            // Shadow bindings (written by ShadowMapFeature when present)
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_SLICES),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT
            });

            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_ATLAS),
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            });

            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_CONFIG),
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            });

            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_SPOT_MAP),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            });

            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_POINT_MAP),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            });

            // EVSM technique resources (binding 9 + 10). Same descriptor set as
            // PCF — only one of the two atlases is written per-frame depending
            // on the active shadow technique. Both bindings are PARTIALLY_BOUND
            // so PCF mode doesn't need to bind them (and vice versa).
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_ATLAS_EVSM),
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            });
            light_bindings.push_back({
                .binding = static_cast<uint32_t>(ELightSetBindings::SHADOW_EVSM_CONFIG),
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
            });

            // Per-frame descriptor sets — mark UPDATE_AFTER_BIND (see Instance set comment).
            // Shadow bindings also get PARTIALLY_BOUND since they may remain
            // unwritten when ShadowMapFeature is not used.
            std::vector<VkDescriptorBindingFlags> light_bind_flags(
                light_bindings.size(), VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
            // Shadow bindings 4-10 are optional — mark PARTIALLY_BOUND
            light_bind_flags[4]  |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            light_bind_flags[5]  |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            light_bind_flags[6]  |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            light_bind_flags[7]  |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            light_bind_flags[8]  |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            light_bind_flags[9]  |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            light_bind_flags[10] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

            VkDescriptorSetLayoutBindingFlagsCreateInfo light_bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            light_bf.bindingCount  = static_cast<uint32_t>(light_bind_flags.size());
            light_bf.pBindingFlags = light_bind_flags.data();

            VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            create_info.bindingCount = static_cast<uint32_t>(light_bindings.size());
            create_info.pBindings    = light_bindings.data();
            create_info.pNext        = &light_bf;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[LIGHT_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 4: Materials (SSBO) — 3 per-family bindings
        {
            std::vector<VkDescriptorSetLayoutBinding> material_bindings;
            
            for (uint32_t i = 0; i < kMaterialFamilyBindingCount; ++i) {
                material_bindings.push_back({
                    .binding = i,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
                });
            }

            // Per-frame descriptor sets — mark UPDATE_AFTER_BIND (see Instance set comment).
            std::vector<VkDescriptorBindingFlags> mat_bind_flags(
                material_bindings.size(), VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);

            VkDescriptorSetLayoutBindingFlagsCreateInfo mat_bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            mat_bf.bindingCount  = static_cast<uint32_t>(mat_bind_flags.size());
            mat_bf.pBindingFlags = mat_bind_flags.data();

            VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            create_info.bindingCount = static_cast<uint32_t>(material_bindings.size());
            create_info.pBindings    = material_bindings.data();
            create_info.pNext        = &mat_bf;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[MATERIAL_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 5: Particle SSBO (GPU simulation + billboard vertex read)
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = static_cast<uint32_t>(EParticleSetBindings::PARTICLES);
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;

            VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.bindingCount = 1;
            create_info.pBindings    = &binding;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[PARTICLE_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 6: Compute cull (GPU-driven) — 1 UBO + 3 SSBO, COMPUTE stage only
        {
            std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
            // binding 0: CullUBO (frustum planes)
            bindings[0].binding         = static_cast<uint32_t>(EComputeSetBindings::CULL_UBO);
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            // binding 1: instance data SSBO
            bindings[1].binding         = static_cast<uint32_t>(EComputeSetBindings::INSTANCE_DATA);
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            // binding 2: indirect draw commands SSBO
            bindings[2].binding         = static_cast<uint32_t>(EComputeSetBindings::DRAW_COMMANDS);
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            // binding 3: draw counts / visible indices SSBO
            bindings[3].binding         = static_cast<uint32_t>(EComputeSetBindings::VISIBLE_INDICES);
            bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo create_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.bindingCount = static_cast<uint32_t>(bindings.size());
            create_info.pBindings    = bindings.data();

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layouts_[COMPUTE_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        // SET 7: Bindless vertex source array (R1.4 of render-refactor).
        // One SSBO descriptor slot per vertex pool — readable from VERTEX_BIT
        // (mesh shaders read vertices), COMPUTE_BIT (skinning + future
        // producers write skinned output), and FRAGMENT_BIT (rare — but
        // included for completeness so deferred passes can sample attributes).
        // PARTIALLY_BOUND + UPDATE_AFTER_BIND so registered pools sit at
        // their assigned indices and the slot can be re-written as pools
        // come and go.
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = static_cast<uint32_t>(EVertexPoolSetBindings::VERTEX_POOLS);
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = kVertexPoolMaxCount;
            binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT
                                    | VK_SHADER_STAGE_FRAGMENT_BIT
                                    | VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorBindingFlags bf_flags =
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            bf.bindingCount  = 1;
            bf.pBindingFlags = &bf_flags;

            VkDescriptorSetLayoutCreateInfo create_info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            create_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            create_info.bindingCount = 1;
            create_info.pBindings    = &binding;
            create_info.pNext        = &bf;

            if (vkCreateDescriptorSetLayout(device, &create_info, nullptr,
                                            &layouts_[VERTEX_POOL_IDX]) != VK_SUCCESS) {
                return false;
            }
        }

        return true;
    }

    std::vector<VkDescriptorSetLayout> GeneralDescriptorSetLayout::getAllLayouts() const
    {
        return { layouts_.begin(), layouts_.end() };
    }
}
