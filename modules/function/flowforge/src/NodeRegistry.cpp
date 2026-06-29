#include <lux/engine/flowforge/NodeRegistry.hpp>
#include <lux/engine/flowforge/ControlNode.hpp>

namespace lux::flowforge
{
	template<typename T> requires std::is_base_of_v<lux::flowforge::Node, T>
	std::unique_ptr<NodeCreatInfo> createControlFlowCreator(std::string_view name)
	{
		auto ptr = std::make_unique<NodeCreatInfo>();
		ptr->name = name;
		ptr->category = "Control Flow";
		ptr->creator = []() -> std::unique_ptr<Node>
		{
			return std::make_unique<T>();
		};

		return ptr;
	}

	NodeRegistry::NodeRegistry()
	{
		registerBuiltinNodes();
	}

	NodeRegistry::~NodeRegistry()
	{

	}

	void NodeRegistry::registerBuiltinNodes()
	{
		registerNode(createControlFlowCreator<StartNode>("Start"));
		registerNode(createControlFlowCreator<BranchNode>("Branch"));
		registerNode(createControlFlowCreator<SequenceNode>("Sequence"));
		registerNode(createControlFlowCreator<ForLoopNode>("For Loop"));
		registerNode(createControlFlowCreator<WhileLoopNode>("While Loop"));
	}

	bool NodeRegistry::registerNode(std::unique_ptr<NodeCreatInfo> info)
	{
		if (!info)
		{
			return false;
		}

		auto [it, inserted] = node_name_map_.try_emplace(info->name, info.get());
		if (!inserted)
		{
			return false;
		}
		node_category_map_[info->category].push_back(info.get());
		node_creators_.push_back(std::move(info));

		return true;
	}

	NodeCreatInfo* NodeRegistry::findNodeByName(const std::string& name) const
	{
		auto it = node_name_map_.find(name);
		return it != node_name_map_.end() ? it->second : nullptr;
	}

	NodeCreatInfo* NodeRegistry::findNodeByCategory(const std::string& name) const
	{
		auto it = node_category_map_.find(name);
		return it != node_category_map_.end() ? it->second.front() : nullptr;
	}
}