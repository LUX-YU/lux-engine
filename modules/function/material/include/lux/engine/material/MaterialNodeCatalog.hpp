#pragma once

#include <lux/engine/function/graph/GraphNodeTypeIdentity.hpp>
#include <lux/engine/material/MaterialNodePayload.hpp>
#include <lux/engine/material/graph/Types.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace lux::material
{
    class MaterialGraph;
    inline constexpr std::uint32_t kMaterialNodeAbi = 2;

    enum class EMaterialNodeRole : std::uint8_t
    {
        VALUE,
        SURFACE
    };

    enum class EMaterialInputUse : std::uint8_t
    {
        VALUE,
        CONNECTED_VALUE,
        UNUSED
    };

    struct MaterialPinDeclaration final
    {
        graph::PinSemanticId semantic;
        std::string name;
        graph::EPinDirection direction{graph::EPinDirection::INPUT};
        EValueType type{EValueType::FLOAT};
        std::array<float, 4> default_value{};
        // Input demand is independent of authoring pin existence. UNUSED is never evaluated;
        // CONNECTED_VALUE leaves an unconnected input as an empty expression instead of creating a constant.
        EMaterialInputUse input_use{EMaterialInputUse::VALUE};
    };

    struct MaterialNodeRegistration final
    {
        using PinResult = MaterialNodeResult<std::vector<MaterialPinDeclaration>>;
        using Create = MaterialNodeResult<MaterialNodePayload> (*)(const object::CodeLease&) noexcept;
        using DescribePins = PinResult (*)(const MaterialNodePayload&) noexcept;
        using Validate = MaterialNodeResult<void> (*)(const MaterialNodePayload&) noexcept;
        using ValidateBindings =
            MaterialNodeResult<void> (*)(const MaterialNodePayload&, const MaterialGraph&) noexcept;
        using EmitSlang = MaterialNodeResult<void> (*)(
            const MaterialNodePayload&,
            std::span<const std::string_view> inputs,
            std::span<const std::string_view> outputs,
            std::string& destination
        ) noexcept;
        using Encode = MaterialNodeResult<std::string> (*)(const MaterialNodePayload&) noexcept;
        using Decode = MaterialNodeResult<MaterialNodePayload> (*)(std::string_view, const object::CodeLease&) noexcept;

        graph::GraphNodeTypeIdentity identity;
        cxx::basic_type_info payload_type;
        object::CodeLease code{object::CodeLease::builtin()};
        Create create{};
        // Authoring schema admission is independent of compilation eligibility. This callback must
        // reject malformed payload fields before using them to construct pin declarations.
        DescribePins describe_pins{};
        // Intrinsic compilation diagnostics; an editable draft need not pass this check.
        Validate validate{};
        EmitSlang emit_slang{};
        EMaterialNodeRole role{EMaterialNodeRole::VALUE};
        // Optional resource admission, including disconnected nodes. Does not emit shader expressions.
        ValidateBindings validate_bindings{};
        // Optional paired source codec for this exact identity/version. No compiler eligibility requirement.
        Encode encode{};
        Decode decode{};
    };

    // Immutable definition. A borrowed use of this object and its payload cannot overlap their destruction.
    // Keeping this shared definition permits execution after the contributing catalog is destroyed.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialNodeType final
    {
    public:
        MaterialNodeType(const MaterialNodeType&) = delete;
        MaterialNodeType& operator=(const MaterialNodeType&) = delete;
        MaterialNodeType(MaterialNodeType&&) = delete;
        MaterialNodeType& operator=(MaterialNodeType&&) = delete;
        ~MaterialNodeType();

        [[nodiscard]] const graph::GraphNodeTypeIdentity& identity() const noexcept;
        [[nodiscard]] EMaterialNodeRole role() const noexcept;
        [[nodiscard]] MaterialNodeResult<MaterialNodePayload> create() const noexcept;
        [[nodiscard]] MaterialNodeResult<void> validate(const MaterialNodePayload&) const noexcept;
        [[nodiscard]] MaterialNodeResult<void>
        validateBindings(const MaterialNodePayload&, const MaterialGraph&) const noexcept;
        // Checks payload ownership and authoring schema, without requiring a compilable node.
        [[nodiscard]] MaterialNodeRegistration::PinResult describePins(const MaterialNodePayload&) const noexcept;
        [[nodiscard]] MaterialNodeResult<std::string> encode(const MaterialNodePayload&) const noexcept;
        [[nodiscard]] MaterialNodeResult<MaterialNodePayload> decode(std::string_view) const noexcept;

        // Inputs/outputs follow declaration order. Empty inputs represent absent/UNUSED values.
        // Output identifiers name predeclared typed locals. All views remain stable during emission;
        // callbacks borrow them and append statements, never retain them or the destination.
        [[nodiscard]] MaterialNodeResult<void>
        emitSlang(
            const MaterialNodePayload&,
            std::span<const std::string_view> inputs,
            std::span<const std::string_view> outputs,
            std::string& destination
        ) const noexcept;

    private:
        friend class MaterialNodeCatalog;
        explicit MaterialNodeType(MaterialNodeRegistration) noexcept;
        [[nodiscard]] bool accepts(const MaterialNodePayload&) const noexcept;

        std::string payload_type_name_;
        MaterialNodeRegistration registration_;
    };

    enum class EMaterialNodeCatalogError : std::uint8_t
    {
        INVALID_REGISTRATION,
        DUPLICATE_TYPE,
        HASH_COLLISION
    };

    // Compose on one thread, then lend immutable definitions to workers. Lookup does not construct payloads.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialNodeCatalog final
    {
    public:
        MaterialNodeCatalog() noexcept = default;
        ~MaterialNodeCatalog();
        MaterialNodeCatalog(const MaterialNodeCatalog&) = delete;
        MaterialNodeCatalog& operator=(const MaterialNodeCatalog&) = delete;
        MaterialNodeCatalog(MaterialNodeCatalog&&) = delete;
        MaterialNodeCatalog& operator=(MaterialNodeCatalog&&) = delete;

        // Binary plugins must negotiate this ABI before constructing a callback table. Old entry
        // symbols must not be invoked with the new registration layout (see the versioned sample).
        [[nodiscard]] cxx::expected<void, EMaterialNodeCatalogError>
        add(std::span<const MaterialNodeRegistration>, std::uint32_t abi = kMaterialNodeAbi) noexcept;

        [[nodiscard]] std::shared_ptr<const MaterialNodeType> find(graph::NodeTypeId) const noexcept;

    private:
        std::vector<std::shared_ptr<const MaterialNodeType>> types_;
    };
} // namespace lux::material
