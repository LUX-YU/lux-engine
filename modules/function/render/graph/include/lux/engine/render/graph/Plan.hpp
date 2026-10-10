#pragma once

#include <lux/engine/render/graph/Definition.hpp>

namespace lux::render
{
    struct GraphResourceLifetime
    {
        std::uint32_t first_pass;
        std::uint32_t last_pass;
        bool operator==(const GraphResourceLifetime&) const noexcept = default;
    };

    struct LogicalCompileOptions
    {
        bool cull_unused{true};
        bool allow_local_read{false};
        bool operator==(const LogicalCompileOptions&) const noexcept = default;
    };

    struct ShaderResourceBinding
    {
        GraphResourceId resource;
        rdesc::EPassFieldRole role;
        VGraphRange range;
        std::string dimension, image_format;
        std::uint32_t array_count, element_stride;
        bool descriptor_array;
        bool operator==(const ShaderResourceBinding&) const noexcept = default;
    };

    struct SamplerBinding
    {
        std::string paired_texture;
        bool operator==(const SamplerBinding&) const noexcept = default;
    };

    struct AttachmentBinding
    {
        GraphResourceId resource;
        ImageRange range;
        ELoadOp load, stencil_load;
        EStoreOp store, stencil_store;
        rdesc::EPassFieldRole role;
        std::string paired_texture;
        bool operator==(const AttachmentBinding&) const noexcept = default;
    };

    struct TransferBinding
    {
        GraphResourceId resource;
        VGraphRange range;
        rdesc::EPassFieldRole role;
        bool operator==(const TransferBinding&) const noexcept = default;
    };

    using VGraphBinding = std::variant<ShaderResourceBinding, SamplerBinding, AttachmentBinding, TransferBinding>;

    struct GraphFieldBinding
    {
        std::string path, shader_name, semantic;
        std::uint32_t array_element, stages;
        rdesc::EFieldOwner owner;
        rdesc::EUpdateFrequency frequency;
        bool required;
        VGraphBinding value;
        bool operator==(const GraphFieldBinding&) const noexcept = default;
    };

    struct LogicalPass
    {
        std::vector<GraphResourceUse> uses;
        PassKey key;
        ShaderKey shader;
        EPassKind kind;
        EExecutionScope scope;
        std::string canonical_name, shader_name, schema_name, shader_declarations;
        std::vector<GraphScalarField> scalar_fields;
        std::vector<GraphFieldBinding> bindings;
        std::size_t scalar_size{};
        GraphResourceKey condition;
        std::uint32_t invocation_inputs{};
        bool operator==(const LogicalPass&) const noexcept = default;
    };

    struct LogicalGraphIdentity
    {
        std::vector<GraphResource> resources;
        std::vector<LogicalPass> passes;
        std::vector<GraphDependency> dependencies;
        std::vector<GraphOutput> outputs;
        std::vector<GraphProvider> providers;
        LogicalCompileOptions options;
        bool operator==(const LogicalGraphIdentity&) const noexcept = default;
    };

    enum class EGraphHazard
    {
        RAW,
        WAR,
        WAW,
        ORDER,
        FALLBACK
    };

    struct GraphHazard
    {
        GraphPassId before, after;
        GraphResourceId resource;
        VGraphRange range;
        EGraphHazard kind;
    };

    struct LogicalResourceVersion
    {
        GraphResourceId resource;
        VGraphRange range;
        GraphPassId writer; // zero is an initialized imported version
        std::vector<GraphPassId> readers;
        std::optional<GraphResourceLifetime> lifetime;
    };

    struct LogicalInputChoice
    {
        GraphPassId consumer;
        std::uint32_t use_index;
        GraphPassId conditional_producer;
        GraphResourceId fallback;
    };

    struct LogicalReuseCandidate
    {
        std::uint32_t before_version, after_version;
    };

    struct GraphCompileDiagnostic
    {
        std::optional<RenderError> error;
        std::vector<PassKey> cycle_path;
        std::vector<std::string> cycle_names;
        std::string json;
    };

    struct LogicalGraphPlanData
    {
        LogicalGraphIdentity identity;
        LogicalGraphIdentity cache_identity;
        std::vector<GraphPassId> order;
        std::vector<GraphDependency> dependencies;
        std::vector<GraphHazard> hazards;
        std::vector<LogicalResourceVersion> versions;
        std::vector<std::optional<GraphResourceLifetime>> lifetimes;
        std::vector<GraphResourceId> imports;
        std::vector<bool> live;
        std::vector<bool> scene_share_eligible;
        std::vector<LogicalReuseCandidate> reuse_candidates;
        std::vector<LogicalInputChoice> input_choices;
        std::vector<std::vector<GraphPassId>> scene_sources;
        bool requires_invocation_data{false};
        std::string diagnostics_json;
    };

    class LogicalGraphPlan;
    [[nodiscard]] RenderResult<LogicalGraphPlan> compileLogicalGraph(
        const RenderGraphDefinition& definition,
        const LogicalCompileOptions& options = {}
    ) noexcept;
    [[nodiscard]] GraphCompileDiagnostic diagnoseLogicalGraph(
        const RenderGraphDefinition& definition,
        const LogicalCompileOptions& options = {}
    ) noexcept;
    [[nodiscard]] LogicalGraphIdentity logicalIdentity(
        const RenderGraphDefinition& definition,
        const LogicalCompileOptions& options
    ) noexcept;

    [[nodiscard]] std::vector<std::string> explainLogicalCompatibility(
        const LogicalGraphPlan& plan,
        const RenderGraphDefinition& candidate
    ) noexcept;

    // Complete immutable logical result. Logical intervals are never GPU retirement/alias evidence.
    class LogicalGraphPlan
    {
    public:
        [[nodiscard]] const LogicalGraphIdentity& identity() const noexcept
        {
            return data_.identity;
        }

        [[nodiscard]] std::span<const GraphPassId> executionOrder() const noexcept
        {
            return data_.order;
        }

        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept
        {
            return data_.dependencies;
        }

        [[nodiscard]] std::span<const std::optional<GraphResourceLifetime>> lifetimes() const noexcept
        {
            return data_.lifetimes;
        }

        [[nodiscard]] std::span<const GraphResourceId> imports() const noexcept
        {
            return data_.imports;
        }

        [[nodiscard]] std::span<const GraphHazard> hazards() const noexcept
        {
            return data_.hazards;
        }

        [[nodiscard]] std::span<const LogicalResourceVersion> versions() const noexcept
        {
            return data_.versions;
        }

        [[nodiscard]] std::span<const LogicalReuseCandidate> reuseCandidates() const noexcept
        {
            return data_.reuse_candidates;
        }

        [[nodiscard]] const std::vector<bool>& livePasses() const noexcept
        {
            return data_.live;
        }

        [[nodiscard]] const std::vector<bool>& sceneShareEligible() const noexcept
        {
            return data_.scene_share_eligible;
        }

        [[nodiscard]] std::span<const LogicalInputChoice> inputChoices() const noexcept
        {
            return data_.input_choices;
        }

        [[nodiscard]] const auto& sceneSources() const noexcept
        {
            return data_.scene_sources;
        }

        [[nodiscard]] bool requiresInvocationData() const noexcept
        {
            return data_.requires_invocation_data;
        }

        [[nodiscard]] const LogicalGraphIdentity& cacheIdentity() const noexcept
        {
            return data_.cache_identity;
        }

        // Diagnostic lookup hint only; matches() always checks complete typed identity.
        [[nodiscard]] std::uint64_t fingerprint() const noexcept
        {
            return cxx::Fnv1a64::hash(data_.diagnostics_json);
        }

        [[nodiscard]] std::string_view diagnosticsJson() const noexcept
        {
            return data_.diagnostics_json;
        }

        [[nodiscard]] bool matches(const RenderGraphDefinition& candidate) const noexcept
        {
            return data_.cache_identity == logicalIdentity(candidate, data_.identity.options);
        }

    private:
        friend RenderResult<LogicalGraphPlan>
        compileLogicalGraph(const RenderGraphDefinition&, const LogicalCompileOptions&) noexcept;

        explicit LogicalGraphPlan(LogicalGraphPlanData data) noexcept : data_(std::move(data)) {}

        LogicalGraphPlanData data_;
    };
} // namespace lux::render
