#pragma once

#include <memory>
#include <string>

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::ui
{
	struct PaneFactory final
	{
		using CreateFuncType = std::unique_ptr<Pane> (lux::object::ObjectDispatcherRef, PaneId);
		using CreatePaneFunctionType = lux::cxx::move_only_function<CreateFuncType>;

		PaneTypeId 				type;
		std::string 			display_name;
		CreatePaneFunctionType 	create;
	};
} // namespace lux::ui
