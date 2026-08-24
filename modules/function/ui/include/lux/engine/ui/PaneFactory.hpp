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
        using Create = lux::cxx::move_only_function<std::unique_ptr<Pane>(
            lux::object::ObjectDispatcherRef, PaneId)>;

        PaneTypeId type;
        std::string display_name;
        Create create;
    };
} // namespace lux::ui
