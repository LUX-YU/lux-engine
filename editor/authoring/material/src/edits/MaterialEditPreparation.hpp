#pragma once
#include <lux/engine/editor/material/MaterialEdit.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <algorithm>
#include <cmath>
#include <optional>

namespace lux::editor::material::detail
{
    inline auto rejected(EMaterialEditError code, lux::material::NodeId node = {})
    {
        return lux::cxx::unexpected(MaterialEditError{code, node});
    }
    inline MaterialEditError historyFailure(editing::EditFailure failure) noexcept
    {
        MaterialEditError result{EMaterialEditError::HISTORY};
        result.history = failure;
        return result;
    }
    using VMaterialValue = std::variant<
        MaterialRename,
        MaterialSetConstant,
        MaterialSetShading,
        MaterialSetRenderState,
        MaterialSetTextureSlots,
        MaterialSetParameterSlots>;
    struct MaterialValueEdit final
    {
        std::vector<VMaterialValue> before, after;
        [[nodiscard]] MaterialEditResult<void> set(const lux::material::MaterialSource&, VMaterialValue value);
        [[nodiscard]] bool empty() const noexcept
        {
            return after.empty();
        }
        [[nodiscard]] std::size_t bytes() const noexcept;
        void normalize();
    };
    [[nodiscard]] MaterialEditResult<VMaterialValue>
    readValue(const lux::material::MaterialSource&, const VMaterialValue&);
    [[nodiscard]] bool equalValue(const VMaterialValue&, const VMaterialValue&) noexcept;
    void swapValue(lux::material::MaterialSource&, VMaterialValue&) noexcept;
    [[nodiscard]] std::size_t nodeBytes(const lux::material::Node&) noexcept;
    [[nodiscard]] std::size_t sourceBytes(const lux::material::MaterialSource&) noexcept;
    [[nodiscard]] MaterialEditResult<void> validateReferences(
        const lux::material::MaterialSource& before,
        const lux::material::MaterialGraph& graph,
        std::span<const lux::material::TextureSlotDecl> textures,
        std::span<const lux::material::ParamSlotDecl> parameters
    );
    struct MaterialGraphDelta final
    {
        std::vector<std::unique_ptr<lux::material::Node>> insert;
        std::vector<lux::material::NodeId> erase;
        std::vector<lux::graph::LinkRecord> connect, disconnect;
        std::vector<lux::graph::GraphLayoutEntry> place;
        std::vector<lux::material::NodeId> unplace;
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t bytes() const noexcept;
    };
    [[nodiscard]] MaterialEditResult<void> applyGraphEdit(
        lux::material::MaterialSource&,
        VMaterialEdit&,
        std::vector<lux::material::NodeId>& inserted
    );
    [[nodiscard]] MaterialEditResult<void> graphDifference(
        const lux::material::MaterialGraph&,
        const lux::material::MaterialGraph&,
        MaterialGraphDelta& before,
        MaterialGraphDelta& after
    );
}
