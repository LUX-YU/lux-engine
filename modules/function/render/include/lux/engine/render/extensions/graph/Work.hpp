#pragma once

#include <array>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <lux/engine/render/extensions/graph/ParameterSource.hpp>

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
    };

    struct ValueParameter final
    {
        std::string shader_field;
        GraphResourceId resource; // Invalid selects the instance parameter block.
        std::string source_field;
    };

    enum class EDeviceParameter : std::uint8_t
    {
        INDIRECT_FIRST_INSTANCE
    };

    struct DeviceParameter final
    {
        std::string shader_field;
        EDeviceParameter source = EDeviceParameter::INDIRECT_FIRST_INSTANCE;
    };

    struct ParameterBinding final
    {
        std::vector<std::byte> values;
        rdesc::ShaderBlock instance_layout;
        std::vector<ValueParameter> fields;
        std::vector<DeviceParameter> device_fields;
        // Explicitly zero-initialize any reflected tail beyond the supplied constant bytes.
        bool zero_fill_tail = false;
        EParameterSource source = EParameterSource::INSTANCE;
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
        // Shaderless typed UAV writes (R32_UINT), in private descriptor order.
        std::vector<ResourceAccess> typed_buffer_writes;
    };

    enum class EAttachmentLoad : std::uint8_t
    {
        DISCARD,
        CLEAR,
        LOAD
    };

    enum class EAttachmentStore : std::uint8_t
    {
        DISCARD,
        STORE
    };

    struct RasterAttachment final
    {
        ResourceAccess access;
        EAttachmentLoad load = EAttachmentLoad::CLEAR;
        EAttachmentStore store = EAttachmentStore::STORE;
        std::array<float, 4> clear{};
        float clear_depth = 1;
        std::optional<ResourceAccess> resolve;
    };

    struct RasterState final
    {
        RasterViewport viewport;
        float depth_bias_constant = 0, depth_bias_slope = 0;
    };

    struct RasterStateInput final
    {
        GraphResourceId resource;
        std::uint32_t offset = 0;
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
        };

        std::string purpose;
        std::vector<Input> inputs;
        // Fixed graph inputs keep their ordinary hazard/alias analysis. Actual external inputs
        // are resolved from the slots above and participate in the frame's imported dependencies.
        std::vector<ResourceAccess> resources;
        ParameterBinding parameters;
    };

    struct RasterScope
    {
        [[nodiscard]] bool hasAttachments() const noexcept
        {
            return !colors.empty() || depth.has_value();
        }

        std::vector<RasterAttachment> colors;
        std::optional<RasterAttachment> depth;
        // Per-pass rectangle in its attachment; absent uses the instance's normalized viewport.
        std::optional<RasterViewport> viewport;
        std::optional<RasterStateInput> state_input; // Prepared CPU parameter record; overrides viewport and bias.
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

    struct ImageCopyRegion final
    {
        std::uint32_t x = 0, y = 0, width = 0, height = 0;
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

    using VGraphCommand = std::variant<RasterWork, ComputeWork, TransferWork>;

    enum class EConditionSource : std::uint8_t
    {
        INPUT,
        HISTORY_VALID
    };

    struct PassCondition final
    {
        EConditionSource source = EConditionSource::INPUT;
        // INPUT: declared condition index. HISTORY_VALID: the Previous resource of a declared pair.
        std::uint32_t input = 0;
        bool expected = true;
        bool operator==(const PassCondition&) const noexcept = default;
    };

    struct GraphPassOptions final
    {
        bool side_effect = false;
        std::optional<PassCondition> condition;
    };

    struct PassResult final
    {
        GraphPassId pass;
        std::vector<ResourceVersion> written;
    };
} // namespace lux::render
