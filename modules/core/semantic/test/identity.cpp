#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdio>
#include <cstdlib>
#include <string_view>

struct Identity final
{
    std::string_view name;
    lux::semantic::TypeId id;
};

// Frozen protocol identities from the implementation preceding MA02.
constexpr Identity Identities[]{
    {"lux.bool", 0x47017e82b8f66068ULL},
    {"lux.i32", 0xa2bcfc75d3d77326ULL},
    {"lux.u32", 0x8083f075c09413faULL},
    {"lux.i64", 0xa2ce1475d3e60f21ULL},
    {"lux.u64", 0x8079e075c08ba445ULL},
    {"lux.f32", 0x12f2a37613495f5bULL},
    {"lux.f64", 0x12fc9376135198b0ULL},
    {"lux.simulation.SimulationStepInfo", 0xe1693768c2b2a642ULL},
    {"lux.simulation.ScriptEndPlayReason", 0x120bcf12f116fc13ULL},
    {"lux.scene.script.AssetReadOutcome.v1", 0x728d70805bee2754ULL},
    {"lux.resource.AssetId.v1", 0x68e50e357eb34aeaULL},
    {"lux.scene.script.AssetHandle.v1", 0x7c8a1b0655a66bbcULL},
    {"lux.scene.script.AssetInspection.v1", 0x03d34584811c4e4cULL},
    {"lux.scene.script.AssetBytes.v1", 0xfea0ec523daeab71ULL},
};

static_assert(
    []() noexcept
    {
        for (const auto& value : Identities)
        {
            if (lux::semantic::typeId(value.name) != value.id)
            {
                return false;
            }
        }
        return true;
    }()
);

static_assert(lux::semantic::BuiltinLayouts.size() == 7U);

int main()
{
    for (const auto& layout : lux::semantic::BuiltinLayouts)
    {
        bool found{};
        for (const auto& identity : Identities)
        {
            if (layout.canonical_name == identity.name)
            {
                found = layout.type_id == identity.id;
            }
        }
        if (!found)
        {
            std::abort();
        }
    }
    for (const auto& value : Identities)
    {
        std::printf(
            "%.*s %016llx\n",
            static_cast<int>(value.name.size()),
            value.name.data(),
            static_cast<unsigned long long>(lux::semantic::typeId(value.name))
        );
    }
}
