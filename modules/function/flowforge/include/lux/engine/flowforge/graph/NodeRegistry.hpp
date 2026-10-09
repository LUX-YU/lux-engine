#pragma once

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/flowforge/graph/FlowNode.hpp>

#include <unordered_map>

namespace lux::meta
{
    class ReflectionRegistry;
}

namespace lux::flowforge
{
    struct NodeCreateInfo final
    {
        using Creator = cxx::move_only_function<FlowForgeResult<FlowNode>() noexcept>;

        std::string name;
        std::string category;
        Creator creator;
    };

    // UI-facing creation recipes, not a second topology or node-type authority.
    // Each factory constructs a plain payload using a retained, registered definition.
    class LUX_ENGINE_FLOWFORGE_PUBLIC NodeRegistry final
    {
    public:
        // Dynamic providers pin their recipe/registration code independently of native callee code.
        [[nodiscard]] static FlowForgeResult<std::unique_ptr<NodeRegistry>>
        create(object::CodeLease code = object::CodeLease::builtin()) noexcept;

        NodeRegistry(const NodeRegistry&) = delete;
        NodeRegistry& operator=(const NodeRegistry&) = delete;
        NodeRegistry(NodeRegistry&&) = delete;
        NodeRegistry& operator=(NodeRegistry&&) = delete;

        ~NodeRegistry();

        [[nodiscard]] bool registerNode(std::unique_ptr<NodeCreateInfo>) noexcept;

        // Free reflected functions with scalar/pointer signatures. Immutable native definitions
        // own the copied metadata and code lease; the reflection registry need not outlive nodes.
        std::size_t populateFromReflection(const meta::ReflectionRegistry&, object::CodeLease) noexcept;

        [[nodiscard]] NodeCreateInfo* findNodeByName(const std::string&) const noexcept;
        [[nodiscard]] NodeCreateInfo* findNodeByCategory(const std::string&) const noexcept;

        [[nodiscard]] const std::vector<std::unique_ptr<NodeCreateInfo>>& nodeCreators() const noexcept
        {
            return node_creators_;
        }

    private:
        explicit NodeRegistry(object::CodeLease) noexcept;
        void registerBuiltinNodes(object::CodeLease) noexcept;

        std::shared_ptr<const FlowNodeType> native_type_;
        std::vector<std::unique_ptr<NodeCreateInfo>> node_creators_;
        std::unordered_map<std::string, NodeCreateInfo*> node_name_map_;
        std::unordered_map<std::string, std::vector<NodeCreateInfo*>> node_category_map_;
    };
} // namespace lux::flowforge
