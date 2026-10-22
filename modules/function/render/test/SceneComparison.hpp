#pragma once

#include "ClusterCompare.hpp"
#include "SceneCapture.hpp"

// Exact comparison of the actual RGBA16F images before exposure, quantization or Tonemap.
struct SceneComparison
{
    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        GraphImageDesc image{64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        image.extent.target_relative = true;
        ports.resources.push_back({"reference", EFeaturePort::INPUT, image});
        ports.resources.push_back({"candidate", EFeaturePort::INPUT, image});
        ports.resources.push_back({"differences", EFeaturePort::OUTPUT, GraphBufferDesc{64 * 64 * 4}});
        return {};
    }

    static RenderResult<void> build(const void*, std::string_view key, FeatureBuildContext& context) noexcept
    {
        rdesc::SpirvArtifact shader;
        shader.stages.push_back(
            {rdesc::EShaderType::COMPUTE, "main", {std::begin(kClusterCompare), std::end(kClusterCompare)}}
        );
        CHECK(rdesc::ShaderInfo::deserialize(kClusterCompareReflection, shader.interface));
        ComputeWork work;
        work.interface.program = take(context.graph.addProgram(std::move(shader)));
        work.interface.resources = {
            {"reference_image", {context.resource("reference")}},
            {"candidate_image", {context.resource("candidate")}},
            {"differences", {context.resource("differences"), {}, EGraphAccess::WRITE}}
        };
        work.dispatch = DispatchArguments{{8, 8, 1}};
        auto compared =
            context.graph.addComputePass(featureKey(FeatureName{"test.Compare"}, key, "compare"), std::move(work));
        if (!compared)
        {
            return lux::cxx::unexpected(compared.error());
        }
        return context.publish("differences", compared->written[0]);
    }
};

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareFeature<SceneComparison>(
        RegistrationBatch& batch,
        std::string_view
    ) noexcept
    {
        static const FeatureDescriptor
            descriptor{FeatureName{"test.Compare"}, {}, "HDR pixel comparison", SceneComparison::describe};
        static const FeatureGraphOps graph{SceneComparison::build};
        batch.addFeature({&descriptor, &graph});
        return {};
    }
} // namespace lux::render

static void verifyEqualPixels(const CapturedPixels& pixels)
{
    unsigned observed = 0;
    for (const auto& frame : pixels.frames)
    {
        std::uint32_t marker = 0;
        std::memcpy(&marker, frame.data(), 4);
        if (!marker)
        {
            continue;
        }
        for (std::size_t i = 0; i < 64 * 64; ++i)
        {
            std::memcpy(&marker, frame.data() + i * 4, 4);
            if (marker != 1)
            {
                std::fprintf(stderr, "HDR mismatch pixel=%zu marker=%u\n", i, marker);
            }
            CHECK(marker == 1);
        }
        ++observed;
    }
    CHECK(observed);
}
