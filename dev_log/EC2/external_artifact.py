"""Extra R9 installed fixture: foreign frozen source, arbitrary cooked type, original real IO scenario."""
from pathlib import Path
import json, subprocess, sys
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
s=Path(c['source']);prefix=Path(c['prefix']);root=w/'external-artifact';root.mkdir(exist_ok=True)
build=Path(c['build']).with_name('ec2-final-external-artifact')
(root/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(ec2_external_artifact LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)
find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication)
find_package(lux-engine-editor-material-persistence REQUIRED COMPONENTS material_persistence)
find_package(lux-engine-editor-material-preview REQUIRED COMPONENTS material_preview)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)
add_library(artifact_source SHARED plugin.cpp)
target_link_libraries(artifact_source PRIVATE lux::engine::editor::editor_persistence)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE lux::engine::editor::editor_storage lux::engine::editor::editor_file_publication
    lux::engine::editor::material_persistence lux::engine::editor::material_preview lux::engine::platform::dynamic_library)
target_compile_options(consumer PRIVATE /UNDEBUG /utf-8 /permissive- /Zc:__cplusplus)
enable_testing()
add_test(NAME installed.ec2.foreign_artifact COMMAND consumer "${CMAKE_CURRENT_BINARY_DIR}/files" $<TARGET_FILE:artifact_source>)
''')
(root/'plugin.cpp').write_text('''#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <string>
namespace p = lux::editor::persistence;
class Source final : public p::IArtifactSource
{
public:
    explicit Source(std::string value) : value_(std::move(value)) {}
    p::PersistenceResult<p::EncodedArtifact> encode(std::stop_token stop) const override
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(p::PersistenceFailure{p::EPersistenceError::CANCELLED});
        auto bytes = std::make_shared<const std::string>(value_);
        return p::EncodedArtifact{lux::cxx::SharedBytes<>::fromOwner(bytes, std::as_bytes(std::span(*bytes)))};
    }
private:
    std::string value_;
};
extern "C" __declspec(dllexport) p::IArtifactSource* makeSource(const char* bytes, std::size_t size)
{
    return new Source{std::string(bytes, size)};
}
''')
body=(s/'editor/tests/integration/project/artifact_publication.cpp').read_text()
body='#include <lux/engine/dynamic_library/DynamicLibrary.hpp>\n#include <lux/engine/editor/persistence/DerivedArtifact.hpp>\n'+body
body=body.replace('assert(argc == 2);','assert(argc == 3);')
old='''    auto compile = take(em::MaterialCompileOperation::start(runtime, take(model->capture())));
    wait([&] { return compile->ready(); });
    auto compiled = take(compile->result());
    auto artifact = take(em::captureMaterialArtifact(compiled));'''
new='''    auto library = std::make_shared<engine::platform::DynamicLibrary>(argv[2]);
    assert(library->is_loaded());
    using Factory = p::IArtifactSource* (*)(const char*, std::size_t);
    auto factory = reinterpret_cast<Factory>(library->get_symbol("makeSource"));
    assert(factory);
    const auto stamp = model->describe().current;
    auto foreign = std::shared_ptr<const p::IArtifactSource>(factory(initial.data(), initial.size()));
    auto cooked = std::make_shared<const std::string>("external arbitrary cooked bytes");
    const auto make_artifact = [&] {
        return p::DerivedArtifact{contracts::CodeLease::plugin(library),
            {stamp, id, "example.ec2.foreign.result", 1, 0x45433258},
            cxx::SharedBytes<>::fromOwner(cooked, std::as_bytes(std::span(*cooked))), foreign};
    };
    auto artifact = make_artifact();'''
assert body.count(old)==1
body=body.replace(old,new).replace('auto stale = take(em::captureMaterialArtifact(compiled));','auto stale = make_artifact();')
body=body.replace('EC2 artifact: real compile, source save, Unknown, manifest failure, fixed retry, closed author',
    'EC2 external artifact: actual DLL encoder, arbitrary cooked type, source save, Unknown, manifest failure, fixed retry, closed author')
(root/'main.cpp').write_text(body)
for name,args in [('configure',['cmake','-S',root,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    '-DCMAKE_PREFIX_PATH='+prefix.as_posix(),'-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake']),
    ('build',['cmake','--build',build,'--target','all','-j','4','--','-k','0']),
    ('no-work',['cmake','--build',build,'--target','all','-j','4','--','-k','0']),
    ('test',['ctest','--test-dir',build,'--output-on-failure','-j','1'])]:
    subprocess.run([sys.executable,str(w/'run.py'),'--runtime',str(prefix/'bin'),'--cwd',str(s),
        'final-external-artifact-'+name,*map(str,args)],check=True)
