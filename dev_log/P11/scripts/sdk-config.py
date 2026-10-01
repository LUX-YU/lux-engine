from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11');base=s/'cmake/installed-consumers/editor-p11'
p=base/'Extension.cpp';t=p.read_text().replace('#include <stdexcept>','#include <stdexcept>\n#include <lux/engine/editor/scene/ConfigurationForm.hpp>\n#include <lux/engine/meta/TypeStaticInfo.hpp>');pos=t.index('using namespace lux;');t=t[:pos]+'''struct Configuration final { bool enabled{true}; float scale{2.0f}; };
namespace lux::meta
{
    template<> struct TTypeStaticInfo<Configuration>
    {
        static constexpr bool available=true;
        static constexpr auto fields=std::make_tuple(typeStaticField<&Configuration::enabled>("enabled"),
            typeStaticField<&Configuration::scale>("scale"));
    };
}
void registerConfiguration(lux::meta::ReflectionRegistry& registry,lux::meta::qual_type_index_fix_list&)
{
    auto value=std::make_unique<lux::meta::RefClass>();
    value->name="Configuration";value->full_name=lux::cxx::type_name<Configuration>();
    value->hash=lux::cxx::type_hash<Configuration>();value->type=lux::meta::ref_type_of_v<Configuration>;
    value->type.ptr=value.get();
    value->construct=[](void* data) { std::construct_at(static_cast<Configuration*>(data)); };
    value->destruct=[](void* data) { std::destroy_at(static_cast<Configuration*>(data)); };
    registry.registerClass(std::move(value));
}
''' +t[pos:];t=t.replace('        using namespace commands;','''        using namespace commands;
        draft.reflection.push_back({code,&registerConfiguration});
        auto configuration=lux::editor::detail::configurationEditor<Configuration>("qualification.configuration");
        configuration.code=code;draft.configurations.push_back(std::move(configuration));''');t=t.replace('{1,1,1,0,0}', '{1,1,1,1,0,1}');p.write_text(t)
p=base/'main.cpp';t=p.read_text().replace('#include <lux/engine/ui/Root.hpp>','#include <lux/engine/ui/Root.hpp>\n#include <lux/engine/ui/Layout.hpp>');t=t.replace('    persistence::SaveId save_id;', '''    persistence::SaveId save_id;
    ui::Pane configuration_window{messages.dispatcherRef(),ui::PaneId{"configuration"},ui::PaneTypeId{"configuration"},"Configuration"};
    ui::Layout configuration_layout{configuration_window,ui::ElementId{"content"},ui::ELayoutType::VERTICAL};
    configuration_window.setContent(configuration_layout);
    std::optional<lux::editor::scene::ConfigurationControl> configuration;''');t=t.replace('        const auto current = catalog.snapshot();','''        const auto current = catalog.snapshot();
        assert(current.configurations().size()==1);
        configuration.emplace(take(lux::editor::scene::makeConfigurationControl(current.configurations()[0],
            configuration_layout,ui::ElementId{"fields"},{})));''');t=t.replace('    assert(weak_library.expired() && facts.unloaded==1);','''    assert(!weak_library.expired() && facts.unloaded==0); // Configuration outlives the catalogs and file operation.
    std::vector<std::byte> configuration_bytes;
    assert(configuration->encode(configuration_bytes) && !configuration_bytes.empty());
    configuration.reset();
    assert(weak_library.expired() && facts.unloaded==1);''');p.write_text(t)
