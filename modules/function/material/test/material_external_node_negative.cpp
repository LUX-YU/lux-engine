#include <lux/engine/material/graph/Node.hpp>

#include <memory>

class ExternalMaterialNode final : public lux::material::Node
{
public:
    ExternalMaterialNode()
        : Node(ConstructionKey{}, lux::material::EMatNodeKind::CONSTANT)
    {
    }

    [[nodiscard]] std::unique_ptr<lux::material::Node> clone() const override
    {
        return std::make_unique<ExternalMaterialNode>();
    }
};

int main()
{
    ExternalMaterialNode node;
    return node.id() == lux::material::invalid_node ? 0 : 1;
}
