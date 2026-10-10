#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <lux/engine/description/PassContract.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Identity.hpp>
#include <lux/engine/render/graph/Authoring.hpp>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lux::render
{
    struct GraphResourceTag;
    struct GraphPassTag;
    using GraphResourceId = cxx::StrongId<GraphResourceTag, std::uint32_t, 0>;
    using GraphPassId = cxx::StrongId<GraphPassTag, std::uint32_t, 0>;

    enum class EGraphResourceKind
    {
        BUFFER,
        IMAGE
    };
    enum class EGraphResourceOrigin
    {
        TRANSIENT,
        IMPORTED
    };
    enum class EGraphAccess
    {
        READ,
        WRITE,
        READ_WRITE
    };
    enum class EGraphUsage
    {
        TRANSFER,
        SHADER,
        COLOR_ATTACHMENT,
        DEPTH_ATTACHMENT,
        VERTEX,
        INDEX,
        UNIFORM,
        PRESENT,
        INDIRECT,
        INPUT_ATTACHMENT,
        RESOLVE
    };

    struct WholeResource
    {
        bool operator==(const WholeResource&) const noexcept = default;
    };

    using VGraphRange = std::variant<WholeResource, ImageRange, BufferRange>;

    // Neutral import boundary. This is a logical initialization promise, never a native fence.
    // Empty initialized_ranges permits writes only until a graph producer supplies a version.
    struct GraphImportContract
    {
        std::vector<VGraphRange> initialized_ranges{WholeResource{}};
        bool temporal_history{false};
        bool operator==(const GraphImportContract&) const noexcept = default;
    };

    // IDs are one-based declaration positions, scoped to this definition. They
    // are neither runtime handles nor identities across unrelated definitions.
    using VGraphResourceDescription = std::variant<BufferDesc, TextureDesc>;

    struct GraphResource
    {
        VGraphResourceDescription description{BufferDesc{}};
        EGraphResourceOrigin origin{EGraphResourceOrigin::TRANSIENT};
        RenderTargetSemanticId target_semantic{};
        EPersistentScope persistent_scope{EPersistentScope::NONE};
        std::string canonical_name;
        GraphResourceKey semantic{};
        std::optional<GraphImportContract> import_contract;

        [[nodiscard]] EGraphResourceKind kind() const noexcept
        {
            return std::holds_alternative<TextureDesc>(description) ? EGraphResourceKind::IMAGE
                                                                    : EGraphResourceKind::BUFFER;
        }

        [[nodiscard]] const TextureDesc& texture() const noexcept
        {
            return std::get<TextureDesc>(description);
        }

        [[nodiscard]] const BufferDesc& buffer() const noexcept
        {
            return std::get<BufferDesc>(description);
        }

        bool operator==(const GraphResource&) const noexcept = default;
    };

    struct AutomaticProducer
    {
        bool operator==(const AutomaticProducer&) const noexcept = default;
    };

    struct ImportedProducer
    {
        bool operator==(const ImportedProducer&) const noexcept = default;
    };

    struct PassProducer
    {
        PassKey pass;
        bool operator==(const PassProducer&) const noexcept = default;
    };

    struct SemanticProducer
    {
        GraphResourceKey semantic;
        bool operator==(const SemanticProducer&) const noexcept = default;
    };

    using VGraphProducer = std::variant<AutomaticProducer, ImportedProducer, PassProducer, SemanticProducer>;

    struct GraphFallback
    {
        GraphResourceId resource;
        VGraphProducer producer{ImportedProducer{}};
        bool operator==(const GraphFallback&) const noexcept = default;
    };

    struct GraphResourceUse
    {
        GraphResourceId resource;
        EGraphAccess access{EGraphAccess::READ};
        EGraphUsage usage{EGraphUsage::SHADER};
        VGraphRange range{WholeResource{}};
        std::uint32_t stages{7};
        std::uint64_t minimum_bytes{};
        std::uint32_t byte_alignment{1};
        std::uint32_t element_stride{};
        std::uint32_t field_index{~0u};
        VGraphProducer producer{AutomaticProducer{}};
        std::optional<GraphFallback> fallback;
        bool local_read{false};

        bool operator==(const GraphResourceUse&) const noexcept = default;
    };

    struct CapturedFieldBinding
    {
        std::string path;
        std::uint32_t array_element{};
        std::string shader_name;
        std::uint32_t array_count{1};
        std::uint32_t element_stride{};
        bool descriptor_array{false};
        std::uint32_t stages{7};
        GraphResourceId resource{};
        GraphSampler sampler{};
        EGraphResourceKind resource_kind{EGraphResourceKind::BUFFER};
        ImageRange image_range{};
        BufferRange buffer_range{};
        ELoadOp load{ELoadOp::DISCARD};
        EStoreOp store{EStoreOp::STORE};
        std::array<float, 4> clear{};
        ELoadOp stencil_load{ELoadOp::DISCARD};
        EStoreOp stencil_store{EStoreOp::DISCARD};
        float clear_depth{1.0f};
        std::uint32_t clear_stencil{};
        rdesc::EPassFieldRole role{rdesc::EPassFieldRole::SAMPLED_READ};
        rdesc::EFieldOwner owner{rdesc::EFieldOwner::PASS_LOCAL};
        rdesc::EUpdateFrequency frequency{rdesc::EUpdateFrequency::FRAME};
        bool required{true};
        std::string semantic;
        std::string paired_texture;
        std::string dimension;
        std::string image_format;

        bool operator==(const CapturedFieldBinding&) const noexcept = default;
    };

    struct GraphScalarField
    {
        std::string path;
        rdesc::EScalarKind kind;
        std::uint32_t offset;
        std::uint32_t size;
        std::uint32_t array_stride;
        std::uint32_t array_count;
        rdesc::EFieldOwner owner;
        rdesc::EUpdateFrequency frequency;
        std::uint32_t stages;

        bool operator==(const GraphScalarField&) const noexcept = default;
    };

    struct GraphPass
    {
        std::vector<GraphResourceUse> uses;
        PassKey key{};
        ShaderKey shader{};
        EPassKind kind{EPassKind::COMPUTE};
        EExecutionScope scope{EExecutionScope::VIEW};
        std::string canonical_name;
        std::string shader_name;
        std::string schema_name;
        std::string shader_declarations;
        std::vector<GraphScalarField> scalar_fields;
        std::vector<std::byte> initial_scalars;
        std::vector<CapturedFieldBinding> bindings;

        // Zero is unconditional. Nonzero keys identify an atomic conditional group.
        GraphResourceKey condition{};
        // Explicit non-resource inputs: camera=1, render time=2, view history=4, target=8.
        std::uint32_t invocation_inputs{};
        bool operator==(const GraphPass&) const noexcept = default;
    };

    enum class EGraphOutput
    {
        EXPORT,
        PRESENT,
        READBACK,
        EXTERNAL_WRITE
    };

    struct GraphOutput
    {
        GraphResourceId resource;
        VGraphProducer producer{AutomaticProducer{}};
        VGraphRange range{WholeResource{}};
        EGraphOutput kind{EGraphOutput::EXPORT};
        GraphResourceKey semantic{};
        bool operator==(const GraphOutput&) const noexcept = default;
    };

    struct GraphProvider
    {
        GraphResourceKey semantic;
        GraphResourceId resource;
        PassKey pass;
        bool operator==(const GraphProvider&) const noexcept = default;
    };

    struct GraphDependency
    {
        GraphPassId before;
        GraphPassId after;

        bool operator==(const GraphDependency&) const noexcept = default;
    };

    inline constexpr auto kGraphInvalidResource = error::errorId("lux.render.graph.invalid_resource");
    inline constexpr auto kGraphInvalidUse = error::errorId("lux.render.graph.invalid_use");
    inline constexpr auto kGraphInvalidDependency = error::errorId("lux.render.graph.invalid_dependency");
    inline constexpr auto kGraphCycle = error::errorId("lux.render.graph.cycle");
    inline constexpr auto kGraphMissingProducer = error::errorId("lux.render.graph.missing_producer");
    inline constexpr auto kGraphInvalidBinding = error::errorId("lux.render.graph.invalid_binding");

    inline constexpr auto kGraphAmbiguousProducer = error::errorId("lux.render.graph.ambiguous_producer");

    inline constexpr auto kGraphScopeConflict = error::errorId("lux.render.graph.scope_conflict");

    inline constexpr auto kGraphConditionalInput = error::errorId("lux.render.graph.conditional_input");

    inline constexpr auto kGraphInvalidOutput = error::errorId("lux.render.graph.invalid_output");

    inline constexpr auto kGraphInvalidImport = error::errorId("lux.render.graph.invalid_import");

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept;

    // Owns declarations, not execution objects. Construction validates all IDs
    // and usage combinations; compile separately proves scheduling legality.
    class RenderGraphBuilder;

    namespace detail
    {
        struct DefinitionAccess;
    }

    class RenderGraphDefinition
    {
    private:
        friend class RenderGraphBuilder;
        friend struct detail::DefinitionAccess;

        [[nodiscard]] static RenderResult<RenderGraphDefinition> create(
            std::vector<GraphResource> resources,
            std::vector<GraphPass> passes,
            std::vector<GraphDependency> dependencies = {},
            std::vector<GraphOutput> outputs = {},
            std::vector<GraphProvider> providers = {}
        ) noexcept;

    public:
        [[nodiscard]] std::span<const GraphResource> resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] std::span<const GraphPass> passes() const noexcept
        {
            return passes_;
        }

        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept
        {
            return dependencies_;
        }

        [[nodiscard]] std::span<const GraphOutput> outputs() const noexcept
        {
            return outputs_;
        }

        [[nodiscard]] std::span<const GraphProvider> providers() const noexcept
        {
            return providers_;
        }

        // Actual owning value comparison: exact value equality, no hash collisions,
        // simulation revision, frame time, backing tokens or dynamic offsets.
        bool operator==(const RenderGraphDefinition&) const noexcept = default;

    private:
        RenderGraphDefinition(
            std::vector<GraphResource> resources,
            std::vector<GraphPass> passes,
            std::vector<GraphDependency> dependencies,
            std::vector<GraphOutput> outputs,
            std::vector<GraphProvider> providers
        ) noexcept;

        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
        std::vector<GraphOutput> outputs_;
        std::vector<GraphProvider> providers_;
    };
} // namespace lux::render
