#pragma once

#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/flowforge/FlowNodePayload.hpp>
#include <lux/engine/flowforge/FlowValueCompiler.hpp>
#include <lux/engine/flowforge/graph/FlowSourceData.hpp>
#include <lux/engine/function/graph/GraphNodeTypeIdentity.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>

#include <span>
#include <string>
#include <vector>

namespace lux::flowforge
{
    struct FlowNode;
    struct FlowSourceEnvironment;

    // Declarations are restored before bodies, within the same unpublished graph candidate.
    enum class EFlowSourceStage : std::uint8_t
    {
        DECLARATION,
        BODY
    };
    class FlowExecutionCompiler;

    // Borrowed only for a synchronous candidate validation call. Lookups see the complete
    // candidate, including replacements and removals; no mutable graph or persistent cache escapes.
    struct FlowReferenceView final
    {
        cxx::function_ref<const FlowNode*(graph::NodeId) noexcept> node;
        cxx::function_ref<const meta::RefType*(std::uint64_t) noexcept> variable_type;
    };

    enum class EFlowPinRole : std::uint8_t
    {
        DATA,
        EXECUTION
    };

    enum class EFlowValueEvaluation : std::uint8_t
    {
        PURE,
        // Re-evaluate on every use, including within the same control-flow region.
        READS_STATE
    };

    struct FlowPinDeclaration final
    {
        using InitialValue =
            FlowForgeResult<meta::RuntimeObject> (*)(const FlowNodePayload&, const meta::RefType&) noexcept;

        graph::PinSemanticId semantic;
        std::string name;
        graph::EPinDirection direction{graph::EPinDirection::INPUT};
        // Borrowed immutable metadata; its environment must outlive all definitions and graphs using it.
        const meta::RefType* type{};
        bool allow_default{};
        EFlowPinRole role{EFlowPinRole::DATA};
        bool necessary{};
        // Fresh data-input value, prepared once; null (or an empty successful value) leaves it link-only.
        // allow_default governs editing, not initialization. Restore keeps the captured value.
        // The node definition pins callback code; commit never invokes it.
        InitialValue initial_value{};
    };

    // A definition supplies exactly one value or control compiler. Neither role erases a legacy Node.
    struct FlowNodeRegistration final
    {
        using PinResult = FlowForgeResult<std::vector<FlowPinDeclaration>>;
        using ValueResult = FlowForgeResult<std::vector<FlowValue>>;
        using Create = FlowForgeResult<FlowNodePayload> (*)(const object::CodeLease&) noexcept;
        using DescribePins = PinResult (*)(const FlowNodePayload&) noexcept;
        using Validate = FlowForgeResult<void> (*)(const FlowNodePayload&) noexcept;
        using Compile =
            ValueResult (*)(const FlowNodePayload&, std::span<const FlowValue>, FlowValueCompiler&) noexcept;
        using CaptureSource = FlowSourceResult<VFlowSourceParameters> (*)(const FlowNodePayload&) noexcept;
        using RestoreResult = FlowSourceResult<FlowNodePayload>;
        using RestoreSource = RestoreResult (*)(
            const FlowSourceNode&,
            const FlowSourceEnvironment&,
            FlowReferenceView,
            const object::CodeLease&
        ) noexcept;
        using ValidateReferences = FlowForgeResult<void> (*)(const FlowNodePayload&, FlowReferenceView) noexcept;
        using CompileExecution = FlowForgeResult<void> (*)(const FlowNodePayload&, FlowExecutionCompiler&) noexcept;

        graph::GraphNodeTypeIdentity identity;
        cxx::TypeToken payload_type;
        object::CodeLease code{object::CodeLease::builtin()};
        Create create{};
        DescribePins describe_pins{};
        Validate validate{};
        Compile compile{};
        // Synchronous source conversion only; callbacks cannot retain source/environment/reference views.
        // No pair means an explicitly unsavable definition. A half pair is rejected during registration.
        CaptureSource capture_source{};
        RestoreSource restore_source{};
        ValidateReferences validate_references{};
        CompileExecution compile_execution{};
        EFlowValueEvaluation value_evaluation{EFlowValueEvaluation::PURE};
        EFlowSourceStage source_stage{EFlowSourceStage::BODY};
    };

    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodeType final
    {
    public:
        FlowNodeType(const FlowNodeType&) = delete;
        FlowNodeType& operator=(const FlowNodeType&) = delete;
        FlowNodeType(FlowNodeType&&) = delete;
        FlowNodeType& operator=(FlowNodeType&&) = delete;
        ~FlowNodeType();

        [[nodiscard]] const graph::GraphNodeTypeIdentity& identity() const noexcept;
        [[nodiscard]] FlowForgeResult<FlowNodePayload> create() const noexcept;
        [[nodiscard]] FlowForgeResult<void> validate(const FlowNodePayload&) const noexcept;
        [[nodiscard]] FlowForgeResult<void>
        validateReferences(const FlowNodePayload&, FlowReferenceView) const noexcept;
        [[nodiscard]] FlowNodeRegistration::PinResult describePins(const FlowNodePayload&) const noexcept;
        [[nodiscard]] EFlowSourceStage sourceStage() const noexcept;
        [[nodiscard]] FlowSourceResult<VFlowSourceParameters> captureSource(const FlowNodePayload&) const noexcept;
        [[nodiscard]] FlowSourceResult<FlowNodePayload> restoreSource(
            const FlowSourceNode&,
            const FlowSourceEnvironment&,
            FlowReferenceView
        ) const noexcept;
        // A failed callback invalidates the caller's disposable compile candidate. Never publish it.
        [[nodiscard]] FlowNodeRegistration::ValueResult
        compile(const FlowNodePayload&, std::span<const FlowValue>, FlowValueCompiler&) const noexcept;
        [[nodiscard]] bool hasExecutionCompiler() const noexcept;
        [[nodiscard]] EFlowValueEvaluation valueEvaluation() const noexcept;
        [[nodiscard]] FlowForgeResult<void>
        compileExecution(const FlowNodePayload&, FlowExecutionCompiler&) const noexcept;

    private:
        friend class FlowNodeCatalog;
        explicit FlowNodeType(FlowNodeRegistration) noexcept;
        [[nodiscard]] bool accepts(const FlowNodePayload&) const noexcept;

        std::string payload_type_name_;
        FlowNodeRegistration registration_;
    };

    enum class EFlowNodeCatalogError : std::uint8_t
    {
        INVALID_REGISTRATION,
        DUPLICATE_TYPE,
        HASH_COLLISION
    };

    // Single-threaded composition; published definitions are immutable and retain their code.
    // The catalog never constructs payloads during registration or lookup.
    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodeCatalog final
    {
    public:
        FlowNodeCatalog() noexcept = default;
        ~FlowNodeCatalog();
        FlowNodeCatalog(const FlowNodeCatalog&) = delete;
        FlowNodeCatalog& operator=(const FlowNodeCatalog&) = delete;
        FlowNodeCatalog(FlowNodeCatalog&&) = delete;
        FlowNodeCatalog& operator=(FlowNodeCatalog&&) = delete;

        [[nodiscard]] cxx::expected<void, EFlowNodeCatalogError> add(std::span<const FlowNodeRegistration>) noexcept;
        [[nodiscard]] std::shared_ptr<const FlowNodeType> find(graph::NodeTypeId) const noexcept;

    private:
        std::vector<std::shared_ptr<const FlowNodeType>> types_;
    };

} // namespace lux::flowforge
