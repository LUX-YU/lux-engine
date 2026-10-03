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
        contracts::CodeLease code = contracts::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeMaterialSessionFactory(
        contracts::CodeLease code = contracts::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeNewMaterialCommand(
        commands::CommandEntry::Query,
        sessions::SessionCreation
    );
} // namespace lux::editor::material
