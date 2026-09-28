#pragma once
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
namespace lux::editor::flowforge
{
    using FlowSessionAccess = sessions::TSessionAccess<FlowSession>;
}
