from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11');b=s/'cmake/installed-consumers/editor-p11'
p=b/'main.cpp';t=p.read_text().replace('assert(argc == 8);','assert(argc == 9);\n    const bool with_configuration=std::string_view{argv[8]}=="with-configuration";').replace('index != argc;','index != 8;');t=t.replace('        auto snapshot = take(extensions::ContributionSnapshot::prepare(take(extension.contributions())));','''        auto draft=take(extension.contributions());
        if(!with_configuration) {draft.configurations.clear();draft.reflection.clear();}
        auto snapshot = take(extensions::ContributionSnapshot::prepare(std::move(draft)));''');t=t.replace('        assert(current.configurations().size()==1);\n        configuration.emplace(take(lux::editor::scene::makeConfigurationControl(current.configurations()[0],\n            configuration_layout,ui::ElementId{"fields"},{})));','''        assert(current.configurations().size()==(with_configuration ? 1 : 0));
        if(with_configuration) configuration.emplace(take(lux::editor::scene::makeConfigurationControl(
            current.configurations()[0],configuration_layout,ui::ElementId{"fields"},{})));''');t=t.replace('    assert(!weak_library.expired() && facts.unloaded==0); // Configuration outlives the catalogs and file operation.\n    std::vector<std::byte> configuration_bytes;\n    assert(configuration->encode(configuration_bytes) && !configuration_bytes.empty());\n    configuration.reset();','''    if(with_configuration)
    {
        assert(!weak_library.expired() && facts.unloaded==0); // Configuration outlives the catalogs and file operation.
        std::vector<std::byte> configuration_bytes;
        assert(configuration->encode(configuration_bytes) && !configuration_bytes.empty());configuration.reset();
    }''');p.write_text(t)
p=b/'CMakeLists.txt';t=p.read_text().replace('add_test(NAME installed.p11.extension COMMAND p11_consumer','foreach(mode IN ITEMS with-configuration without-configuration)\nadd_test(NAME installed.p11.extension.${mode} COMMAND p11_consumer').replace('"$<TARGET_FILE:p11_reject_10>")','"$<TARGET_FILE:p11_reject_10>" "${mode}")\nendforeach()');p.write_text(t)
