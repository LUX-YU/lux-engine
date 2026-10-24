#include <lux/engine/platform/AtomicFileWriter.hpp>

#include <array>
#include <cassert>
#include <utility>

int main(int argc, char** argv)
{
    using namespace lux::platform;
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    const auto path = directory / "published.bin";
    const std::array original{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array replacement{std::byte{4}, std::byte{5}, std::byte{6}};
    assert(writeFileAtomic(path, original, EFileWriteMode::REPLACE));

    auto create = AtomicFileWriter::create(path, EFileWriteMode::CREATE);
    assert(create && create->write(replacement));
    const auto duplicate = create->commit();
    assert(!duplicate && duplicate.error().code == EFileIoError::DESTINATION_EXISTS);
    assert(readFileBounded(path, 3).value() == std::vector(original.begin(), original.end()));

    {
        // Two live transactions have distinct temporary files; cancellation and destruction preserve the target.
        auto first = AtomicFileWriter::create(path, EFileWriteMode::REPLACE);
        auto second = AtomicFileWriter::create(path, EFileWriteMode::REPLACE);
        assert(first && second && first->write(replacement) && second->write(replacement));
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = first->commit(stop.get_token());
        assert(!cancelled && cancelled.error().code == EFileIoError::CANCELLED);
        assert(readFileBounded(path, 3).value() == std::vector(original.begin(), original.end()));
    }

    {
        auto first = AtomicFileWriter::create(path, EFileWriteMode::REPLACE);
        auto second = AtomicFileWriter::create(path, EFileWriteMode::REPLACE);
        assert(first && second && first->write(original) && second->write(replacement));
        *first = std::move(*second);
        auto moved = std::move(*first);
        assert(moved.seek(1));
        const std::array backfill{std::byte{9}};
        assert(moved.write(backfill) && moved.commit());
    }
    const std::vector expected{std::byte{4}, std::byte{9}, std::byte{6}};
    assert(readFileBounded(path, 3).value() == expected);
    const auto limited = readFileBounded(path, 2);
    assert(!limited && limited.error().code == EFileIoError::LIMIT);

    const auto blocked = directory / "directory-target";
    std::filesystem::create_directories(blocked);
    assert(!writeFileAtomic(blocked, replacement, EFileWriteMode::REPLACE));
    assert(std::filesystem::is_directory(blocked));
    assert(!AtomicFileWriter::create("relative.bin", EFileWriteMode::CREATE));
}
