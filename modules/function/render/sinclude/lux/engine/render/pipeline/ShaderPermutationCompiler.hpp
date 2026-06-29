#pragma once

#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/pipeline/ShaderPermutation.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <lux/cxx/container/SparseSet.hpp>

namespace lux::render
{
    /**
     * @brief Manages SPIR-V shader registration and variant compilation via
     *        Vulkan specialization constants.
     *
     * Each base shader is registered once with its SPIR-V bytecode and stage.
     * When a specific feature combination is requested, the compiler builds a
     * VkShaderModule using the original SPIR-V plus specialization constant data
     * representing the feature mask bits.  
     *
     * Convention:
     *   - Specialization constant ID 0  →  EShaderFeature bit 0  (HAS_NORMAL_MAP)
     *   - Specialization constant ID 1  →  EShaderFeature bit 1  (HAS_SKINNING)
     *   - ...
     *   - Specialization constant ID 31 →  EShaderFeature bit 31
     *
     * In GLSL:
     *   layout(constant_id = 0) const bool HAS_NORMAL_MAP = false;
     *   layout(constant_id = 1) const bool HAS_SKINNING   = false;
     *   ...
     *
     * The compiler generates VkSpecializationInfo with VK_BOOL32 entries
     * (one per feature) so the driver can dead-code-eliminate unused branches.
     */
    class ShaderPermutationCompiler
    {
    public:
        /// @brief Description of a base (unspecialized) shader.
        struct BaseShader
        {
            std::vector<uint32_t>   spirv_code;             ///< Raw SPIR-V words
            std::string             entry_point = "main";   ///< Entry-point name
            VkShaderStageFlagBits   stage;                  ///< Vertex / Fragment / Compute / ...
        };

        ShaderPermutationCompiler() = default;

        ~ShaderPermutationCompiler()
        {
            if (device_ctx_)
                destroyAll();
        }

        // Non-copyable, movable
        ShaderPermutationCompiler(const ShaderPermutationCompiler&)            = delete;
        ShaderPermutationCompiler& operator=(const ShaderPermutationCompiler&) = delete;

        ShaderPermutationCompiler(ShaderPermutationCompiler&& other) noexcept
            : device_ctx_(other.device_ctx_)
            , next_id_(other.next_id_)
            , base_shaders_(std::move(other.base_shaders_))
            , cache_(std::move(other.cache_))
        {
            other.device_ctx_ = nullptr;
            other.base_shaders_.clear();
            other.cache_.clear();
        }

        ShaderPermutationCompiler& operator=(ShaderPermutationCompiler&& other) noexcept
        {
            if (this != &other)
            {
                if (device_ctx_)
                    destroyAll();
                device_ctx_   = other.device_ctx_;
                next_id_      = other.next_id_;
                base_shaders_ = std::move(other.base_shaders_);
                cache_        = std::move(other.cache_);
                other.device_ctx_ = nullptr;
                other.cache_.clear();
            }
            return *this;
        }

        /**
         * @brief Register a new base shader.
         * @return Unique ID for the shader (used as cache key).
         */
        uint64_t registerBaseShader(const BaseShader& shader)
        {
            std::lock_guard lock(mutex_);
            uint64_t id = next_id_++;
            base_shaders_[id] = shader;
            return id;
        }

        /**
         * @brief Retrieve (or compile & cache) a VkShaderModule for a given
         *        base shader.
         *
         * The same VkShaderModule works for all feature combinations because
         * specialization constants are injected at pipeline creation time,
         * NOT at module creation time. Caching by base_shader_id avoids
         * creating redundant modules per feature combination.
         *
         * Thread-safe. Returns VK_NULL_HANDLE on failure.
         */
        VkShaderModule getOrCompile(DeviceContext& device_ctx, uint64_t base_shader_id)
        {
            device_ctx_ = &device_ctx;  // Remember device context for destructor cleanup
            {
                std::lock_guard lock(mutex_);
                if (cache_.contains(base_shader_id))
                    return cache_.at(base_shader_id);
            }

            // Not cached — compile outside the lock to avoid blocking others
            VkShaderModule module = compileModule(base_shader_id);
            if (module == VK_NULL_HANDLE)
                return VK_NULL_HANDLE;

            {
                std::lock_guard lock(mutex_);
                if (cache_.contains(base_shader_id))
                {
                    // Another thread beat us; destroy our duplicate
                    vkDestroyShaderModule(device_ctx_->logicalDevice(), module, nullptr);
                    return cache_.at(base_shader_id);
                }
                cache_.insert(base_shader_id, module);
            }
            return module;
        }

        /**
         * @brief Build VkSpecializationInfo for a feature mask.
         *
         * The caller must keep the returned SpecializationData alive until
         * pipeline creation is complete (it holds the backing storage).
         */
        struct SpecializationData
        {
            std::vector<VkSpecializationMapEntry> entries;
            std::vector<VkBool32>                 values;
            VkSpecializationInfo                  info{};

            /// @brief Update the info pointers (call after moves / copies of entries/values)
            void finalize()
            {
                info.mapEntryCount = static_cast<uint32_t>(entries.size());
                info.pMapEntries   = entries.data();
                info.dataSize      = values.size() * sizeof(VkBool32);
                info.pData         = values.data();
            }
        };

        static SpecializationData buildSpecializationData(ShaderFeatureMask features, uint32_t max_features = 32)
        {
            SpecializationData sd;
            sd.entries.reserve(max_features);
            sd.values.reserve(max_features);

            for (uint32_t i = 0; i < max_features; ++i)
            {
                VkSpecializationMapEntry entry{};
                entry.constantID = i;
                entry.offset     = static_cast<uint32_t>(i * sizeof(VkBool32));
                entry.size       = sizeof(VkBool32);
                sd.entries.push_back(entry);

                sd.values.push_back((features & (1u << i)) ? VK_TRUE : VK_FALSE);
            }
            sd.finalize();
            return sd;
        }

        /**
         * @brief Destroy all cached shader modules.
         *
         * Uses the stored DeviceContext to obtain VkDevice.
         * If no DeviceContext was set, destruction is skipped.
         */
        void destroyAll()
        {
            std::lock_guard lock(mutex_);
            if (device_ctx_)
            {
                for (auto& module : cache_.values())
                    vkDestroyShaderModule(device_ctx_->logicalDevice(), module, nullptr);
            }
            cache_.clear();
            base_shaders_.clear();
        }

        /// @brief Query number of cached modules (for diagnostics)
        std::size_t cachedModuleCount() const
        {
            std::lock_guard lock(mutex_);
            return cache_.size();
        }

        /// @brief Check if a base shader is registered
        bool hasBaseShader(uint64_t id) const
        {
            std::lock_guard lock(mutex_);
            return base_shaders_.contains(id);
        }

    private:
        VkShaderModule compileModule(uint64_t base_shader_id)
        {
            const BaseShader* base = nullptr;
            {
                std::lock_guard lock(mutex_);
                if (!base_shaders_.contains(base_shader_id))
                    return VK_NULL_HANDLE;
                base = &base_shaders_.at(base_shader_id);
            }

            // Create the module from the original SPIR-V.
            // Specialization constants are injected at pipeline creation time,
            // NOT at module creation time — so the same module works for all
            // feature masks.
            VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = base->spirv_code.size() * sizeof(uint32_t);
            ci.pCode    = base->spirv_code.data();

            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device_ctx_->logicalDevice(), &ci, nullptr, &module) != VK_SUCCESS)
                return VK_NULL_HANDLE;

            return module;
        }

        mutable std::mutex mutex_;
        DeviceContext* device_ctx_{ nullptr };  ///< Remembered for destructor cleanup
        uint64_t next_id_{ 1 };

        lux::cxx::SparseSet<uint64_t, BaseShader>     base_shaders_;
        lux::cxx::SparseSet<uint64_t, VkShaderModule> cache_;  ///< base_shader_id -> module
    };

} // namespace lux::render
