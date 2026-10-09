#pragma once

#include <lux/engine/flowforge/FlowNodePayload.hpp>
#include <lux/engine/flowforge/FlowValueCompiler.hpp>
#include <lux/engine/function/graph/GraphNodeTypeIdentity.hpp>

#include <span>
#include <string>
#include <vector>

namespace lux::flowforge
{
    class Node;

    struct FlowPinDeclaration final
    {
        graph::PinSemanticId semantic;
        std::string name;
        graph::EPinDirection direction{graph::EPinDirection::INPUT};
        // Borrowed immutable metadata; its environment must outlive all definitions and graphs using it.
        const meta::RefType* type{};
    };

    // Pure value compilation is the first supported role. Control/native/Ability registrations
    // remain part of the graph migration; they must not be simulated by this scalar interface.
    struct FlowNodeRegistration final
    {
        using PinResult = FlowForgeResult<std::vector<FlowPinDeclaration>>;
        using ValueResult = FlowForgeResult<std::vector<FlowValue>>;
        using Create = FlowForgeResult<FlowNodePayload> (*)(const object::CodeLease&) noexcept;
        using DescribePins = PinResult (*)(const FlowNodePayload&) noexcept;
        using Validate = FlowForgeResult<void> (*)(const FlowNodePayload&) noexcept;
        using Compile =
            ValueResult (*)(const FlowNodePayload&, std::span<const FlowValue>, FlowValueCompiler&) noexcept;
        using Encode = FlowForgeResult<std::string> (*)(const FlowNodePayload&) noexcept;
        using Decode = FlowForgeResult<FlowNodePayload> (*)(std::string_view, const object::CodeLease&) noexcept;

        graph::GraphNodeTypeIdentity identity;
        cxx::TypeToken payload_type;
        object::CodeLease code{object::CodeLease::builtin()};
        Create create{};
        DescribePins describe_pins{};
        Validate validate{};
        Compile compile{};
        Encode encode{};
        Decode decode{};
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
        [[nodiscard]] FlowNodeRegistration::PinResult describePins(const FlowNodePayload&) const noexcept;
        [[nodiscard]] FlowForgeResult<std::string> encode(const FlowNodePayload&) const noexcept;
        [[nodiscard]] FlowForgeResult<FlowNodePayload> decode(std::string_view) const noexcept;
        // A failed callback invalidates the caller's disposable compile candidate. Never publish it.
        [[nodiscard]] FlowNodeRegistration::ValueResult
        compile(const FlowNodePayload&, std::span<const FlowValue>, FlowValueCompiler&) const noexcept;

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

    // Bridges registered semantic payloads into the current graph store. This does not complete
    // the plain NodeId/PinId payload-store migration. The payload never owns an old Node object.
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowForgeResult<std::unique_ptr<Node>> createFlowValueNode(
        std::shared_ptr<const FlowNodeType>,
        FlowNodePayload
    ) noexcept;
} // namespace lux::flowforge
