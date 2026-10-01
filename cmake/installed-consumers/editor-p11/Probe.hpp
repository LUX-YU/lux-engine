#pragma once
#include <lux/engine/editor/sessions/SessionStore.hpp>

namespace probe
{
    struct Facts final
    {
        unsigned queries{}, executions{}, panes_destroyed{}, unloaded{};
        bool fail_query{};
        lux::editor::sessions::SessionStore* sessions{};
    };
    struct Binding final
    {
        Facts* facts;
    };
}
