#pragma once
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
        OwnerAssignment assignment = {}
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
