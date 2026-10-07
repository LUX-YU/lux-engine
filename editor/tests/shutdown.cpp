#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <thread>
int main(int argc, char** argv)
{
    using namespace lux;
    assert(argc == 2);
    auto file = std::filesystem::absolute(argv[1]);
    editor::ProjectManifest manifest{1, *uuids::uuid::from_string("889c1234-0000-0000-0000-000000000000"), "Shutdown"};
    assert(editor::writeProjectManifestAtomic(file, manifest, editor::EProjectWrite::REPLACE));
    auto host = editor::LuxEngine::create({"Posted shutdown", 200, 160});
    assert(host);
    editor::OpenProjectRequest request{file};
    assert(object::sendEvent(**host, request) && !request.rejection.type);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!object::ObjectRuntime::instance().statistics().pending)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::puts("real ObjectScheduler completion POSTED; destroying host");
    std::fflush(stdout);
    host->reset();
    assert(!object::ObjectRuntime::instance().statistics().pending);
    std::puts("host destruction returned; accepted completion settled");
}
