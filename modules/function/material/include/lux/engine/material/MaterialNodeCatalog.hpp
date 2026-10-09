#pragma once

#include <lux/engine/function/graph/GraphNodeTypeIdentity.hpp>
#include <lux/engine/material/MaterialNodePayload.hpp>
#include <lux/engine/material/ShaderIR.hpp>
#include <lux/engine/material/graph/Types.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace lux::material
{
    struct MaterialPinDeclaration final
    {
        graph::PinSemanticId semantic;
        std::string name;
        graph::EPinDirection direction{graph::EPinDirection::INPUT};
        EValueType type{EValueType::FLOAT};
        std::array<float, 4> default_value{};
    };

    struct MaterialNodeRegistration final
    {
        using PinResult = MaterialNodeResult<std::vector<MaterialPinDeclaration>>;
        using ShaderResult = MaterialNodeResult<std::vector<std::uint32_t>>;
        using Create = MaterialNodeResult<MaterialNodePayload> (*)(const object::CodeLease&) noexcept;
        using DescribePins = PinResult (*)(const MaterialNodePayload&) noexcept;
        using Validate = MaterialNodeResult<void> (*)(const MaterialNodePayload&) noexcept;
        using Compile =
            ShaderResult (*)(const MaterialNodePayload&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

        graph::GraphNodeTypeIdentity identity;
        cxx::TypeToken payload_type;
        object::CodeLease code{object::CodeLease::builtin()};
        Create create{};
        DescribePins describe_pins{};
        Validate validate{};
        Compile compile{};
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
        [[nodiscard]] MaterialNodeResult<MaterialNodePayload> create() const noexcept;
        [[nodiscard]] MaterialNodeResult<void> validate(const MaterialNodePayload&) const noexcept;
        [[nodiscard]] MaterialNodeRegistration::PinResult describePins(const MaterialNodePayload&) const noexcept;

        // Inputs refer to already resolved SSA values, in input declaration order. Outputs use output
        // declaration order. The caller owns a disposable IR candidate: failure never authorizes its adoption.
        [[nodiscard]] MaterialNodeRegistration::ShaderResult compile(
            const MaterialNodePayload&,
            std::span<const std::uint32_t> inputs,
            shadergen::ShaderIR& candidate
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

        [[nodiscard]] cxx::expected<void, EMaterialNodeCatalogError> add(std::span<
                                                                         const MaterialNodeRegistration>) noexcept;

        [[nodiscard]] std::shared_ptr<const MaterialNodeType> find(graph::NodeTypeId) const noexcept;

    private:
        std::vector<std::shared_ptr<const MaterialNodeType>> types_;
    };
} // namespace lux::material
