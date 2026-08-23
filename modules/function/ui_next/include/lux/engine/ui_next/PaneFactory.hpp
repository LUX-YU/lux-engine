#pragma once

#include <memory>
#include <string>

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui_next/Pane.hpp>

namespace lux::ui
{
    struct PaneCreateContext final
    {
        lux::object::ObjectDispatcherRef dispatcher;
    };

    struct PaneFactory final
    {
        PaneTypeId type;
        std::string display_name;
        lux::cxx::move_only_function<std::unique_ptr<Pane>(PaneCreateContext, PaneId)>
            create;
    };
} // namespace lux::ui
