#include <cassert>
#include <fstream>
#include <lux/engine/scene/ScenePackageFile.hpp>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace lux;
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
    assert(scene::writeScenePackageAtomic(file, *package, platform::EFileWriteMode::CREATE, limit));
    auto duplicate = scene::writeScenePackageAtomic(file, *package, platform::EFileWriteMode::CREATE, limit);
    assert(
        !duplicate &&
        std::get<platform::FileFailure>(duplicate.error()).code == platform::EFileIoError::DESTINATION_EXISTS
    );
    auto read = scene::readScenePackageFile(file, limit);
    assert(read);
    assert(scene::encodeScenePackage(*read, limit).value() == scene::encodeScenePackage(*package, limit).value());
    auto size_limited = scene::readScenePackageFile(file, 1);
    assert(
        !size_limited && std::get<platform::FileFailure>(size_limited.error()).code == platform::EFileIoError::LIMIT
    );
    std::stop_source stop;
    stop.request_stop();
    auto cancelled =
        scene::writeScenePackageAtomic(file, *package, platform::EFileWriteMode::REPLACE, limit, stop.get_token());
    assert(!cancelled);
    const auto* encoding_cancelled = std::get_if<scene::ScenePackageFailure>(&cancelled.error());
    assert(encoding_cancelled && encoding_cancelled->code == scene::EScenePackageError::CANCELLED);
    auto read_cancelled = scene::readScenePackageFile(file, limit, stop.get_token());
    assert(!read_cancelled);
    const auto* io_cancelled = std::get_if<platform::FileFailure>(&read_cancelled.error());
    assert(io_cancelled && io_cancelled->code == platform::EFileIoError::CANCELLED);
    auto missing = scene::readScenePackageFile(directory / "missing.scene", limit);
    assert(!missing && std::get<platform::FileFailure>(missing.error()).code == platform::EFileIoError::IO);
    {
        std::ofstream corrupt(file, std::ios::binary);
        corrupt << "broken package";
    }
    auto broken = scene::readScenePackageFile(file, limit);
    assert(!broken && std::holds_alternative<scene::ScenePackageFailure>(broken.error()));
    assert(scene::writeScenePackageAtomic(file, *package, platform::EFileWriteMode::REPLACE, limit));
    assert(scene::readScenePackageFile(file, limit));
}
