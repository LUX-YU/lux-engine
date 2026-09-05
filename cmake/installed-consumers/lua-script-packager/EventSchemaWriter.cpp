#include <lux/engine/toolchain/lua/ScriptEventSchema.hpp>
#include "InventoryModel.hpp"

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <array>
#include <filesystem>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    auto description = installed_consumer::inventoryDescription();
    if (!description)
        return 1;
    auto event = lux::simulation::script::describeScriptEventSource<std::int32_t>(
        description->findEvent(installed_consumer::InventorySystemId, installed_consumer::ChangedEvent));
    if (!event)
        return 1;
    const std::array sources{*event};
    return lux::toolchain::lua::writeScriptEventSchemaManifest(std::filesystem::path{argv[1]}, sources) ? 0 : 1;
}
