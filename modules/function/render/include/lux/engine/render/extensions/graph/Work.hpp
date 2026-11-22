#pragma once

#include <algorithm>
#include <array>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/extensions/graph/PassValues.hpp>
#include <lux/engine/render/extensions/graph/ParameterBinding.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>

namespace lux::render
{
    struct ResourceParameter final
    {
        std::string shader_name;
        ResourceAccess access;
        // Additional elements of a finite descriptor array, in shader array order.
        std::vector<ResourceAccess> elements;
        std::uint32_t field = kGraphInvalidIndex; // Normalized by Builder; never trusted from author input.
        // A runtime Shader array can be bound to this finite pass-local image list. Zero retains
        // the existing complete device table; nonzero must equal 1 + elements.size().
        std::uint32_t descriptor_count = 0;
        ConstantBlockContract uniform_constant{};

        template <ShaderConstantBlock T> void setUniformConstant(std::uint64_t offset = 0) noexcept
        {
            uniform_constant = kConstantBlockContract<T>;
            access.range = GraphBufferRange{offset, sizeof(T)};
        }
    };

    struct SamplerParameter final
    {
        std::string shader_name;
        // Dense graph-local input slots, beginning at zero. Samplers are immutable borrowed inputs,
        // not memory resources and therefore do not create artificial image accesses or versions.
        std::vector<std::uint32_t> inputs;
        std::uint32_t field = kGraphInvalidIndex;
        bool comparison = false;
    };

    struct PassInterface final
    {
        std::uint32_t program = kGraphInvalidIndex;
        std::vector<ResourceParameter> resources;
        ParameterBinding parameters;
        std::vector<SamplerParameter> samplers;
    };

    struct RasterState final
    {
        RasterViewport viewport;
        float depth_bias_constant = 0, depth_bias_slope = 0;
    };

    // Dynamic programs fill these read-only slots at frame preparation. A slot describes a use,
    // not an allocated Graph resource or a provisional descriptor set/binding.
    struct RasterPassTemplate final
    {
        struct Input final
        {
            std::string name;
            // Vertex/index/indirect and BDA inputs have no descriptor binding.
            std::optional<rdesc::EDescriptorType> kind;
            EGraphUse usage = EGraphUse::UNSPECIFIED;
            // Shader reads use Vertex=1 / Fragment=2. Geometry/indirect stages derive from usage.
            std::uint32_t stages = 0;
            bool comparison_sampler = false;
            // An unbounded read-only table of structured records supplied by the declared Resource.
            bool shared_records = false;
        };

        std::string purpose;
        std::vector<Input> inputs;
        // Fixed graph inputs keep their ordinary hazard/alias analysis. Actual external inputs
        // are resolved from the slots above and participate in the frame's imported dependencies.
        std::vector<ResourceAccess> resources;
        ParameterBinding parameters;

        struct StreamProgram final
        {
            // Index returned by RenderGraphBuilder::addProgram.
            std::uint32_t program = kGraphInvalidIndex;
            GraphicsState graphics;
            ParameterBinding parameters;
            // One source geometry location per shader attribute, or empty for the identity mapping.
            std::vector<std::uint32_t> geometry_locations;
        };

        std::vector<StreamProgram> stream_programs{};
        // Source sampler IDs granted to this stream at link time.
        std::vector<std::uint32_t> stream_samplers{};
        // Requires a depth attachment and an explicit depth bias for every submitted section.
        bool section_depth_bias = false;
        // Select the complete program set before admitting a frame.
        bool runtime_stream_programs = false;
        std::vector<DrawResourceInput> draw_resources;

        [[nodiscard]] bool requiresCapability(EStorageCapability capability) const noexcept
        {
            return std::ranges::any_of(draw_resources, [capability](const auto& input) noexcept
            {
                return (input.required_capabilities & static_cast<std::uint32_t>(capability)) != 0;
            });
        }

        [[nodiscard]] bool usesStreamProducer() const noexcept
        {
            return runtime_stream_programs || !stream_programs.empty();
        }
    };

    struct RasterWork final : RasterScope
    {
        struct FixedProgram final
        {
            PassInterface interface;
            GraphicsState graphics;
            std::vector<ProgramVertexBuffer> vertex_buffers;
            VDrawCommand draw = DrawVertices{};
        };

        using VProgram = std::variant<FixedProgram, RasterPassTemplate>;
        VProgram program;

        // Explicit fixed-program authoring; dynamic producers assign RasterPassTemplate instead.
        [[nodiscard]] FixedProgram& fixed() noexcept
        {
            return std::get<FixedProgram>(program);
        }

        [[nodiscard]] const FixedProgram& fixed() const noexcept
        {
            return std::get<FixedProgram>(program);
        }
    };

    struct DispatchArguments final
    {
        std::array<std::uint32_t, 3> groups{1, 1, 1};
    };

    struct IndirectDispatchArguments final
    {
        ResourceVersion buffer;
        std::uint64_t offset = 0;
    };

    struct ImageDispatchArguments final
    {
        GraphResourceId resource;
        std::uint32_t mip = 0;
        std::array<std::uint32_t, 2> group_size{8, 8};
    };

    using VDispatchArguments = std::variant<DispatchArguments, IndirectDispatchArguments, ImageDispatchArguments>;

    struct ComputeWork final
    {
        PassInterface interface;
        VDispatchArguments dispatch = DispatchArguments{};
        // Permission to use the device's compute queue. Graphics remains a valid fallback.
        bool allow_async_compute = false;
    };

    struct CopyBuffer final
    {
        ResourceAccess source;
        ResourceAccess destination;
    };

    struct CopyImage final
    {
        ResourceAccess source;
        ResourceAccess destination;
        std::optional<ImageCopyRegion> source_region;
        std::optional<ImageCopyRegion> destination_region;
    };

    struct CopyImageBuffer final
    {
        ResourceAccess source;
        ResourceAccess destination;
        // Buffer ranges bound capacity. Fixed regions and row pitches define only the copied rows;
        // padding is not initialized. A destination READ_WRITE explicitly preserves its whole range.
        // Dynamic extents/automatic multi-row pitch require READ_WRITE with an available predecessor.
        std::uint32_t row_pitch = 0;
        std::uint32_t slice_pitch = 0;
        std::optional<ImageCopyRegion> image_region;
    };

    struct FillBuffer final
    {
        ResourceAccess destination;
        std::uint32_t value = 0;
    };

    struct ClearImage final
    {
        ResourceAccess destination;
        std::array<float, 4> color{};
        float depth = 1;
    };

    using VTransferOperation = std::variant<CopyBuffer, CopyImage, CopyImageBuffer, FillBuffer, ClearImage>;

    struct TransferWork final
    {
        std::vector<VTransferOperation> operations;
    };

    struct GraphPassOptions final
    {
        bool side_effect = false;
        std::optional<PassCondition> condition;
        // GPU time for this command on the calibrated workload. A sequence applies the estimate
        // to each dispatch. Zero marks an unknown duration.
        std::uint64_t estimated_gpu_ns = 0;
    };

    struct PassResult final
    {
        GraphPassId pass;
        std::vector<ResourceVersion> written;
    };
} // namespace lux::render
