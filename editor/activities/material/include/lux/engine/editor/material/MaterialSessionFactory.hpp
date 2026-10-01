#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/material/MaterialCodec.hpp>

namespace lux::editor::material
{
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeMaterialSessionFactory(
        contracts::CodeLease code = contracts::CodeLease::builtin()
    );
}
