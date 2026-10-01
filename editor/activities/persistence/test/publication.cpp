#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <span>
#include <chrono>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

using namespace lux::editor;
using namespace persistence;
namespace
{
    auto artifact(std::string_view text)
    {
        auto bytes = std::as_bytes(std::span(text));
        return std::make_shared<const EncodedArtifact>(EncodedArtifact{{bytes.begin(), bytes.end()}});
    }
    std::string read(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    PersistenceResult<void> failAfterReplace(const std::filesystem::path& file, void* counter)
    {
        ++*static_cast<int*>(counter);
        assert(read(file) == "replaced");
        return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::IO, "post-replace durability fault"});
    }
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    // Every test invocation gets its own directory; never overwrite user data.
    root /= std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    storage::FileArtifactStore store(root);
    auto target = store.resolve("Content/a.lux");
    assert(target && target->expected_version == "missing");
    PublicationQuery work{{1}, *target, artifact("first")};
    assert(std::holds_alternative<CommitReceipt>(store.publish(work)));
    assert(read(root / "Content/a.lux") == "first");
    auto alias = store.resolve("Content/../Content/./a.lux");
    assert(alias && alias->key == target->key);
    assert(std::holds_alternative<NotPublished>(store.publish(work)));
    assert(read(root / "Content/a.lux") == "first");
    work = {{2}, *alias, artifact("cancelled")};
    std::stop_source stop;
    stop.request_stop();
    assert(std::holds_alternative<NotPublished>(store.publish(work, stop.get_token())));
    assert(read(root / "Content/a.lux") == "first");
    int calls{};
    storage::FileArtifactStore faulting(root, failAfterReplace, &calls);
    work = {{3}, *alias, artifact("replaced")};
    auto outcome = faulting.publish(work);
    const auto* committed = std::get_if<CommitReceipt>(&outcome);
    assert(committed && committed->warning && committed->durability == EDurability::UNCONFIRMED);
    assert(calls == 1 && read(root / "Content/a.lux") == "replaced");
    assert(!store.resolve("../outside"));
    assert(!store.resolve("Content"));
    // A parent that is a regular file produces a real path/write failure before replacing anything.
    auto invalid = store.resolve("Content/a.lux/child");
    if (invalid)
        assert(std::holds_alternative<NotPublished>(store.publish({{4}, *invalid, artifact("bad")})));
    assert(read(root / "Content/a.lux") == "replaced");
    const auto before_denial = *store.resolve("Content/a.lux");
#if defined(_WIN32)
    assert(SetFileAttributesW((root / "Content/a.lux").c_str(), FILE_ATTRIBUTE_READONLY));
    auto denied = store.publish({{5}, before_denial, artifact("must not replace")});
    assert(std::holds_alternative<NotPublished>(denied));
    assert(std::get<NotPublished>(denied).failure.native_code == ERROR_ACCESS_DENIED);
    assert(SetFileAttributesW((root / "Content/a.lux").c_str(), FILE_ATTRIBUTE_NORMAL));
    auto case_alias = store.resolve("CONTENT/A.LUX");
    assert(case_alias && case_alias->key == before_denial.key);
#else
    std::filesystem::permissions(
        root / "Content",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec
    );
    auto denied = store.publish({{5}, before_denial, artifact("must not replace")});
    assert(std::holds_alternative<NotPublished>(denied));
    std::filesystem::permissions(root / "Content", std::filesystem::perms::owner_all);
#endif
    assert(read(root / "Content/a.lux") == "replaced");
    std::filesystem::create_hard_link(root / "Content/a.lux", root / "alias.lux");
    assert(!store.resolve("alias.lux") && !store.resolve("Content/a.lux"));
    std::filesystem::remove(root / "alias.lux");
    std::cout
        << "Actual filesystem access denial preserves old bytes; case alias and unsupported hard-link aliases PASS\n";
    std::cout << "X05-08 real file create/overwrite/conflict/cancel/path-failure/post-replace durability: PASS\n";
    std::cout << "Evidence directory: " << root << '\n';
}
