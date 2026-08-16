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

	class NodeRegistry
	{
	public:
		NodeRegistry();
		NodeRegistry(const NodeRegistry&) = delete;
		NodeRegistry& operator=(const NodeRegistry&) = delete;

		~NodeRegistry();

		/**
		 * @brief Process-wide shared registry (builtins pre-registered).
		 *        The editor registers its native-call nodes here so the
		 *        graph serializer can re-instantiate them on load — a graph
		 *        file references nodes by creator name, which must resolve
		 *        in whatever process decodes it.
		 */
		static NodeRegistry& global();

		bool registerNode(std::unique_ptr<NodeCreatInfo> info);

		/**
		 * @brief Populates the palette from the reflection registry: every
		 *        FREE function with a callable invoker and a graph-mappable
		 *        signature (scalar / pointer parameters, void or scalar
		 *        return) becomes a NativeFuncCall creator named after the
		 *        function. Methods are skipped for now (Self-pin semantics
		 *        land with the object-model work). Duplicate names are
		 *        skipped (registerNode already refuses them). Returns the
		 *        number of creators added.
		 */
		std::size_t populateFromReflection(const lux::meta::ReflectionRegistry& reflection);

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
