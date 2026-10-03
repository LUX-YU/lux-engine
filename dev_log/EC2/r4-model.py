from pathlib import Path
import subprocess,json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'editor/activities/project/src/AssetImporter.cpp'
t=p.read_text()
start=t.index('                auto parsed = toml::parse(bytes);')
end=t.index('                if (replacement.empty())',start)
t=t[:start]+'''                auto recipe_value = decodeModelImportRecipe(std::as_bytes(std::span(bytes)));
                if (!recipe_value)
                    return lux::cxx::unexpected(std::move(recipe_value.error()));
                Source result{
                    file.parent_path() / recipe_value->root,
                    {recipe_value->entry, {}},
                    recipe_value->configuration
                };
                std::vector<std::string> paths;
                paths.reserve(recipe_value->files.size());
                for (const auto& file : recipe_value->files)
                    paths.push_back(file.path);
'''+t[end:]
t=t.replace('!= digests[index]','!= recipe_value->files[index].digest')
t=t.replace('                toml::array source_files;','                ModelImportRecipe recipe{directory, source->capture.entry, source->config};')
t=t.replace('source_files.push_back(toml::table{{"path", file->path}, {"digest", content_digest}});','recipe.files.push_back({file->path, content_digest});')
start=t.index('                toml::array rotation;')
end=t.index('                std::vector<asset::PakWriteEntry> entries;',start)
t=t[:start]+'''                auto recipe_bytes = encodeModelImportRecipe(recipe);
                if (!recipe_bytes)
                    return lux::cxx::unexpected(std::move(recipe_bytes.error()));
                auto encoded_source = own(std::move(*recipe_bytes));
'''+t[end:]
start=t.index('        lux::cxx::SharedBytes<> own(std::string text)')
end=t.index('        struct Source final',start)
t=t[:start]+t[end:]
for h in ['array','cmath','limits','set','sstream','toml++/toml.hpp']:
 t=t.replace(f'#include <{h}>\n','')
t=t.replace('#include <lux/engine/editor/assets/AssetImporter.hpp>','#include <lux/engine/editor/assets/AssetImporter.hpp>\n#include <lux/engine/editor/assets/ModelImportRecipe.hpp>')
p.write_text(t)
# Only current source/support; archived snapshots remain byte-for-byte unchanged.
files=subprocess.check_output(['git','ls-files','editor','cmake','examples','docs'],cwd=s,text=True).splitlines()
for name in files:
 p=s/name
 if not p.is_file():continue
 try:t=p.read_text()
 except UnicodeDecodeError:continue
 u=t.replace('AssetImporter','ModelImporter').replace('AssetImport','ModelImport')
 if u!=t:p.write_text(u)
for a,b in [('src/AssetImporter.cpp','src/ModelImporter.cpp'),('include/lux/engine/editor/assets/AssetImporter.hpp','include/lux/engine/editor/assets/ModelImporter.hpp')]:
 (s/'editor/activities/project'/a).rename(s/'editor/activities/project'/b)
p=s/'editor/activities/project/CMakeLists.txt';t=p.read_text().replace('SOURCE_FILES src/ModelImporter.cpp)','SOURCE_FILES src/ModelImporter.cpp src/ModelImportRecipe.cpp)')
t=t.replace('    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/assets/ModelImporter.hpp','    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/assets/ModelImportRecipe.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/assets/ModelImporter.hpp')
p.write_text(t)
p=s/'editor/tests/architecture/rules.json';j=json.loads(p.read_text())
for f in ['src/ModelImportRecipe.cpp','include/lux/engine/editor/assets/ModelImportRecipe.hpp']:
 j['editor_layering']['files']['editor/activities/project/'+f]=['editor_assets']
j['editor_layering']['targets']['editor_model_recipe']={'layer':'TEST','role':'TEST','capabilities':['CPU','PROCESS','TOOLCHAIN'],'path':'editor/tests/integration/project'}
j['editor_layering']['files']['editor/tests/integration/project/model_recipe.cpp']=['editor_model_recipe']
p.write_text(json.dumps(j,indent=2)+'\n')
