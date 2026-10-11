#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void recordReadback(
        const char* workload,
        std::uint64_t sample,
        GraphResourceId resource,
        std::span<const std::byte> bytes
    )
    {
        std::printf("READBACK %s sample=%llu resource=%u hex=", workload, sample, resource.value());
        for (const auto byte : bytes)
        {
            std::printf("%02x", std::to_integer<unsigned>(byte));
        }
        std::puts("");
    }

    NativeShaderProgram program(
        const VulkanDevice& device,
        const LogicalGraphPlan& graph,
        GraphPassId pass,
        std::string name,
        lux::rdesc::PassShaderContract schema,
        const std::filesystem::path& directory,
        bool compute,
        std::uint32_t view_mask,
        const GraphicsDescription* graphics_override,
        std::span<const OwnerShape> shared_owners,
        OwnerAssignment assignment
    )
    {
        ShaderBuildInputs inputs{name, "default", "glslc VulkanSDK 1.4.304.0", "vulkan1.3", {}, {}};
        std::vector<ShaderStageBinary> stages;
        if (compute)
        {
            inputs.sources.push_back({name + ".comp.glsl", text(directory / (name + ".comp.glsl"))});
            stages.push_back({4, words(directory / (name + ".comp.spv"))});
        }
        else
        {
            inputs.sources.push_back({name + ".vert.glsl", text(directory / (name + ".vert.glsl"))});
            inputs.sources.push_back({name + ".frag.glsl", text(directory / (name + ".frag.glsl"))});
            stages.push_back({1, words(directory / (name + (view_mask ? ".multi.vert.spv" : ".vert.spv")))});
            if (view_mask)
            {
                inputs.defines.push_back({"F4_MULTIVIEW", "1"});
            }
            stages.push_back({2, words(directory / (name + ".frag.spv"))});
        }
        auto variant = makeCompiledShaderVariant(std::move(inputs), std::move(stages), schema);
        if (!variant)
        {
            std::fprintf(stderr, "%s\n", variant.error().c_str());
        }
        CHECK(variant);
        const std::array schemas{schema};
        std::vector<OwnerShape> owners(shared_owners.begin(), shared_owners.end());
        ShaderOwnerId local;
        if (std::any_of(
                schema.resources.begin(),
                schema.resources.end(),
                [](const auto& value)
                {
                    return lux::rdesc::isShaderDescriptorRole(value.role) &&
                           value.owner == lux::rdesc::EFieldOwner::PASS_LOCAL;
                }
            ))
        {
            owners.push_back(checked(declareOwnerShape(name + ".local", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, schemas)
            ));
            local = owners.back().identity;
        }
        GraphicsDescription graphics;
        graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
        graphics.blends.resize(1);
        graphics.view_mask = view_mask;
        if (graphics_override)
        {
            graphics = *graphics_override;
        }
        VPipelineDescription description =
            compute ? VPipelineDescription{ComputeDescription{}} : VPipelineDescription{graphics};
        return checked(NativeShaderProgram::create(
            device,
            graph,
            pass,
            *variant,
            schema,
            {assignment.scene, assignment.feature, local},
            owners,
            std::move(description)
        ));
    }

} // namespace native_graph_test
