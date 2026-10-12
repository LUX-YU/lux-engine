#pragma once
#include "OwnerAssignments.hpp"
#include "ShaderTestSupport.hpp"
#include <cmath>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/vulkan/graph/Executable.hpp>
#include <lux/engine/render/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/vulkan/transfer/Transfer.hpp>
#include <source_location>

namespace native_graph_test
{
    using namespace foundation_test;
    using namespace shader_test;
    using namespace lux::toolchain;

    template <typename T> T checked(RenderResult<T> value, std::source_location at = std::source_location::current())
    {
        if (!value)
        {
            std::fprintf(
                stderr,
                "NativeGraph:%u error=%llu args=%llu,%llu,%llu\n",
                at.line(),
                static_cast<unsigned long long>(value.error().type),
                value.error().args[0],
                value.error().args[1],
                value.error().args[2]
            );
            std::exit(2);
        }
        return std::move(*value);
    }

    // Test assembly value, not a production compatibility API. Old oracles now
    // explicitly address the immutable plan or the independent execution owner.
    struct NativeGraphFixtureInputs
    {
        const LogicalGraphPlan& logical;
        const VulkanDevice& device;
        const VulkanAllocator& allocator;
        std::array<SubmissionQueue*, 3> queues;
        std::span<const NativePassCommand> commands;
        std::span<const NativeImportBinding> imports;
        const GraphInvocationData& initial_values;
        std::span<const std::pair<GraphSampler, std::reference_wrapper<const Sampler>>> samplers{};
        std::uint32_t frame_capacity{2};
        bool alias{true};
    };

    struct NativeGraphFixture
    {
        ExecutableGraphPlan plan;
        NativeGraphInvocations invocations;

        [[nodiscard]] VkDevice device() const noexcept
        {
            return plan.device();
        }
    };

    inline NativeGraphStatistics fixtureStatistics(const NativeGraphFixture& fixture)
    {
        auto result = fixture.plan.statistics();
        const auto actual = fixture.invocations.statistics();
        result.allocated_bytes = actual.allocated_bytes;
        result.allocations = actual.allocations;
        result.aliased_resources = actual.aliased_resources;
        for (std::size_t i = 0; i < result.compile_ns.size(); ++i)
        {
            result.compile_ns[i] += actual.compile_ns[i];
        }
        return result;
    }

    inline RenderResult<NativeGraphFixture> compileFixture(
        const NativeGraphFixtureInputs& inputs,
        std::vector<NativeShaderProgram> programs
    ) noexcept
    {
        auto plan = compileVulkanGraph(
            {inputs.logical, inputs.device, inputs.queues, inputs.commands, inputs.alias},
            std::move(programs)
        );
        if (!plan)
        {
            return lux::cxx::unexpected(plan.error());
        }
        auto invocations = NativeGraphInvocations::create(
            *plan,
            {inputs.device,
             inputs.allocator,
             inputs.imports,
             inputs.initial_values,
             inputs.samplers,
             inputs.frame_capacity}
        );
        if (!invocations)
        {
            return lux::cxx::unexpected(invocations.error());
        }
        return NativeGraphFixture{std::move(*plan), std::move(*invocations)};
    }

    NativeShaderProgram program(
        const VulkanDevice& device,
        const LogicalGraphPlan& graph,
        GraphPassId pass,
        std::string name,
        lux::rdesc::PassShaderContract schema,
        const std::filesystem::path& directory,
        bool compute,
        std::uint32_t view_mask = 0,
        const GraphicsDescription* graphics_override = nullptr,
        std::span<const OwnerShape> shared_owners = {},
        std::span<const OwnerAssignment> assignment = {}
    );

    void sharedCase(const std::filesystem::path&, Validation&);
    void recordReadback(const char* workload, std::uint64_t sample, GraphResourceId, std::span<const std::byte>);
    void readWriteCase(const std::filesystem::path&, Validation&);
    void copyCase(Validation&);
    void importedCase(const std::filesystem::path&, Validation&);
    void rasterCase(const std::filesystem::path&, Validation&);
    void conditionalCase(const std::filesystem::path&, Validation&);
    std::array<float, 128> multiviewCase(const std::filesystem::path&, Validation&, bool);
    void hzbCase(const std::filesystem::path&, Validation&);
    enum class EAliasScenario
    {
        SERIAL,
        OVERLAP,
        MULTI_QUEUE,
        INCOMPATIBLE
    };
    NativeGraphStatistics aliasCase(
        const std::filesystem::path&,
        Validation&,
        bool,
        EAliasScenario = EAliasScenario::SERIAL
    );
} // namespace native_graph_test
