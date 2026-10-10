#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <lux/engine/description/PassContract.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Identity.hpp>
#include <lux/engine/render/graph/Authoring.hpp>
#include <span>
#include <string>
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

    // IDs are one-based declaration positions, scoped to this definition. They
    // are neither runtime handles nor identities across unrelated definitions.
    struct GraphResource
    {
        EGraphResourceKind kind{EGraphResourceKind::BUFFER};
        EGraphResourceOrigin origin{EGraphResourceOrigin::TRANSIENT};
        RenderTargetSemanticId target_semantic{};
        EPersistentScope persistent_scope{EPersistentScope::NONE};
        TextureDesc texture{};
        BufferDesc buffer{};

        bool operator==(const GraphResource&) const noexcept = default;
    };

    struct GraphResourceUse
    {
        GraphResourceId resource;
        EGraphAccess access{EGraphAccess::READ};
        EGraphUsage usage{EGraphUsage::SHADER};
        bool whole_resource{true};
        ImageRange image_range{};
        BufferRange buffer_range{};
        std::uint32_t stages{7};

        bool operator==(const GraphResourceUse&) const noexcept = default;
    };

    struct GraphFieldBinding
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

        // Initial sampler/clear values are dynamic facts, not logical topology.
        bool operator==(const GraphFieldBinding& other) const noexcept
        {
            return path == other.path && array_element == other.array_element && resource == other.resource &&
                   shader_name == other.shader_name && array_count == other.array_count &&
                   element_stride == other.element_stride && descriptor_array == other.descriptor_array &&
                   stages == other.stages && load == other.load && store == other.store &&
                   stencil_load == other.stencil_load && stencil_store == other.stencil_store && role == other.role &&
                   owner == other.owner && frequency == other.frequency && required == other.required &&
                   semantic == other.semantic && paired_texture == other.paired_texture &&
                   dimension == other.dimension && image_format == other.image_format;
        }
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
        std::string schema_name;
        std::string shader_declarations;
        std::vector<GraphScalarField> scalar_fields;
        std::vector<std::byte> initial_scalars;
        std::vector<GraphFieldBinding> bindings;

        bool operator==(const GraphPass& other) const noexcept
        {
            return uses == other.uses && key == other.key && shader == other.shader && kind == other.kind &&
                   scope == other.scope && schema_name == other.schema_name && bindings == other.bindings &&
                   shader_declarations == other.shader_declarations && scalar_fields == other.scalar_fields;
        }
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
    inline constexpr auto kGraphUnsupportedScheduling = error::errorId("lux.render.graph.unsupported_scheduling");
    inline constexpr auto kGraphInvalidBinding = error::errorId("lux.render.graph.invalid_binding");

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
            std::vector<GraphDependency> dependencies = {}
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

        // Cold topology comparison: exact value equality, no hash collisions,
        // simulation revision, frame time, backing tokens or dynamic offsets.
        bool operator==(const RenderGraphDefinition&) const noexcept = default;

    private:
        RenderGraphDefinition(
            std::vector<GraphResource> resources,
            std::vector<GraphPass> passes,
            std::vector<GraphDependency> dependencies
        ) noexcept;

        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
    };
} // namespace lux::render
