#pragma once

#include <lux/engine/render/graph/Plan.hpp>
#include <lux/engine/render/vulkan/descriptor/Descriptors.hpp>
#include <lux/engine/render/vulkan/pipeline/Graphics.hpp>
#include <lux/engine/render/vulkan/shader/Layout.hpp>
#include <lux/engine/toolchain/shader/ShaderAssets.hpp>

namespace lux::render::vulkan
{
    struct ComputeDescription
    {
        bool operator==(const ComputeDescription&) const noexcept = default;
    };

    using VPipelineDescription = std::variant<ComputeDescription, GraphicsDescription>;

    struct NativeProgramIdentity
    {
        LogicalGraphIdentity graph;
        GraphPassId pass;
        toolchain::ShaderVariantIdentity shader;
        LayoutIdentity layout;
        VPipelineDescription pipeline;
        std::vector<toolchain::ShaderStageBinary> relocated;
        std::uint32_t vendor, device, driver, api;
        bool dynamic_rendering;
        VkDevice device_scope;
        bool operator==(const NativeProgramIdentity&) const noexcept = default;
    };

    // Cold composite candidate. All native ownership remains in the existing R4 RAII types.
    // Caller retains this complete value through its last submission; catalog borrows are not retained.
    class NativeShaderProgram
    {
    public:
        [[nodiscard]] static RenderResult<NativeShaderProgram> create(
            const VulkanDevice& device,
            const LogicalGraphPlan& graph,
            GraphPassId pass,
            const toolchain::CompiledShaderVariant& shader,
            const rdesc::PassShaderContract& schema,
            const OwnerAssignment& assignment,
            std::span<const OwnerShape> owners,
            VPipelineDescription description
        ) noexcept;

        NativeShaderProgram(NativeShaderProgram&&) noexcept = default;
        NativeShaderProgram& operator=(NativeShaderProgram&&) = delete;
        NativeShaderProgram(const NativeShaderProgram&) = delete;
        NativeShaderProgram& operator=(const NativeShaderProgram&) = delete;

        [[nodiscard]] const NativeProgramIdentity& identity() const noexcept
        {
            return identity_;
        }

        [[nodiscard]] VkPipeline native() const noexcept;

        [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept
        {
            return pipeline_layout_.native();
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return pipeline_layout_.device();
        }

        [[nodiscard]] std::span<const DescriptorSetLayout> setLayouts() const noexcept
        {
            return sets_;
        }

    private:
        using VPipeline = std::variant<ComputePipeline, GraphicsPipeline>;
        NativeShaderProgram(
            NativeProgramIdentity identity,
            std::vector<DescriptorSetLayout> sets,
            PipelineLayout layout,
            std::vector<ShaderModule> modules,
            VPipeline pipeline
        ) noexcept;

        NativeProgramIdentity identity_;
        std::vector<DescriptorSetLayout> sets_;
        PipelineLayout pipeline_layout_;
        std::vector<ShaderModule> modules_;
        VPipeline pipeline_;
    };
} // namespace lux::render::vulkan
