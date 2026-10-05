#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/material/MaterialCodec.hpp>

namespace lux::editor::material
{
    [[nodiscard]] sessions::SessionPreparation prepareMaterialSession(
        PreparedMaterialData data,
        sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeMaterialSessionFactory(
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeNewMaterialCommand(
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
} // namespace lux::editor::material
