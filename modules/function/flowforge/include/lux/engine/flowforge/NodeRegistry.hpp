#pragma once
#include <unordered_map>
#include <functional>
#include "NodeBase.hpp"

namespace lux::flowforge
{
	using node_creator = std::function<std::unique_ptr<Node> ()>;

	struct NodeCreatInfo
	{
		std::string  name;
		std::string  category;
		node_creator creator;
	};

	using NodeCreatInfoPtrList		= std::vector<NodeCreatInfo*>;
	using NodeCreatInfoUniquePtrList= std::vector<std::unique_ptr<NodeCreatInfo>>;
	using NodeCreatInfoNameMap		= std::unordered_map<std::string, NodeCreatInfo*>;
	using NodeCreatInfoCategoryMap	= std::unordered_map<std::string, NodeCreatInfoPtrList>;

	class LUX_FUNCTION_PUBLIC NodeRegistry
	{
	public:
		NodeRegistry();
		NodeRegistry(const NodeRegistry&) = delete;
		NodeRegistry& operator=(const NodeRegistry&) = delete;

		~NodeRegistry();

		bool registerNode(std::unique_ptr<NodeCreatInfo> info);

		NodeCreatInfo* findNodeByName(const std::string& name) const;
		NodeCreatInfo* findNodeByCategory(const std::string& name) const;

		const NodeCreatInfoUniquePtrList& node_creators() const { return node_creators_; }

	private:

		void registerBuiltinNodes();

		NodeCreatInfoUniquePtrList	node_creators_;
		NodeCreatInfoNameMap		node_name_map_; // for quick search
		NodeCreatInfoCategoryMap	node_category_map_;
	};
}
