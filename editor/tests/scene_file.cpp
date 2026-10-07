#include <cassert>
#include <fstream>
#include <lux/engine/editor/ScenePackageFile.hpp>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace lux;
    using namespace lux::editor;
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    const auto file = directory / "generic.scene";
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
    auto package = scene::createScenePackage(
        asset::AssetId{ids("scene")},
        "Generic",
        {},
        std::make_shared<const simulation::SimulationDescription>(),
        {}
    );
    assert(package);
    constexpr std::size_t limit = 1024 * 1024;
    std::filesystem::remove(file);
    assert(writeScenePackageAtomic(file, *package, ESceneWrite::CREATE, limit));
    auto duplicate = writeScenePackageAtomic(file, *package, ESceneWrite::CREATE, limit);
    assert(!duplicate && std::get<SceneFileFailure>(duplicate.error()).code == ESceneFileError::DESTINATION_EXISTS);
    auto read = readScenePackageFile(file, limit);
    assert(read);
    assert(scene::encodeScenePackage(*read, limit).value() == scene::encodeScenePackage(*package, limit).value());
    auto small = readScenePackageFile(file, 1);
    assert(!small && std::get<SceneFileFailure>(small.error()).code == ESceneFileError::LIMIT);
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = writeScenePackageAtomic(file, *package, ESceneWrite::REPLACE, limit, stop.get_token());
    assert(!cancelled);
    const auto* encoding_cancelled = std::get_if<scene::ScenePackageFailure>(&cancelled.error());
    assert(encoding_cancelled && encoding_cancelled->code == scene::EScenePackageError::CANCELLED);
    auto read_cancelled = readScenePackageFile(file, limit, stop.get_token());
    assert(!read_cancelled);
    const auto* io_cancelled = std::get_if<SceneFileFailure>(&read_cancelled.error());
    assert(io_cancelled && io_cancelled->code == ESceneFileError::CANCELLED);
    auto missing = readScenePackageFile(directory / "missing.scene", limit);
    assert(!missing && std::get<SceneFileFailure>(missing.error()).code == ESceneFileError::IO);
    {
        std::ofstream corrupt(file, std::ios::binary);
        corrupt << "broken package";
    }
    auto broken = readScenePackageFile(file, limit);
    assert(!broken && std::holds_alternative<scene::ScenePackageFailure>(broken.error()));
    assert(writeScenePackageAtomic(file, *package, ESceneWrite::REPLACE, limit));
    assert(readScenePackageFile(file, limit));
}
