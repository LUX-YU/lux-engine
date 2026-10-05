#pragma once
#include <lux/engine/editor/sessions/SessionStore.hpp>

namespace probe
{
    struct Facts final
    {
        unsigned queries{}, executions{}, panes_destroyed{}, unloaded{};
        unsigned activations{}, activation_queries{}, activation_executions{}, activations_destroyed{};
        bool fail_query{};
        lux::editor::sessions::SessionStore* sessions{};
    };
}
