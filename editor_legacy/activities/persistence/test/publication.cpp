#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/PublicationFileStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <cassert>
#include <algorithm>
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
    void publicationRoots(const std::filesystem::path& directory)
    {
        const auto makeRoot = [&](const char* name)
        {
            const auto path = directory / name;
            std::filesystem::create_directories(path);
            const auto key = storage::publicationTargetKey(path, "root-marker");
            assert(key);
            return std::filesystem::u8path(*key).parent_path();
        };
        const auto project = makeRoot("project");
        const auto user = makeRoot("personal");
        const auto installation = makeRoot("installation");
        storage::PublicationFileStore files(project, user, installation);
        const auto local = files.resolve("Content/payload");
        const auto personal = files.resolve((user / "settings.toml").generic_string());
        assert(local && personal);
        assert(std::holds_alternative<CommitReceipt>(files.publish({{11}, *local, artifact("project")})));
        assert(std::holds_alternative<CommitReceipt>(files.publish({{12}, *personal, artifact("personal")})));
        assert(read(project / "Content/payload") == "project");
        assert(read(user / "settings.toml") == "personal");
        const auto alias = files.resolve((user / "sub/../settings.toml").generic_string());
        assert(alias && alias->key == personal->key);
        assert(std::holds_alternative<NotPublished>(files.publish({{13}, *personal, artifact("stale")})));
        assert(!files.resolve((directory / "outside").generic_string()));
        assert(!files.resolve((user.parent_path() / "personal-sibling/payload").generic_string()));
        assert(!files.resolve((user / "../outside").generic_string()));
        const auto installed_bytes = std::as_bytes(std::span("installed", 9));
        assert(storage::writePublicationFile(installation / "settings.toml", installed_bytes));
        const auto installed = files.resolve((installation / "settings.toml").generic_string());
        assert(installed);
        PublicationQuery forbidden{{14}, *installed, artifact("must not replace")};
        const auto refused = files.publish(forbidden);
        assert(std::get<NotPublished>(refused).failure.code == EPersistenceError::UNSUPPORTED_TARGET);
        const auto checked = files.reconcile(forbidden);
        assert(checked.writer_retired);
        assert(std::get<NotPublished>(checked.outcome).failure.code == EPersistenceError::UNSUPPORTED_TARGET);
        forbidden.action = EPublicationAction::REMOVE;
        assert(std::holds_alternative<NotPublished>(files.publish(forbidden)));
        assert(read(installation / "settings.toml") == "installed");
        // A project below the installation directory retains its explicitly granted project root.
        const auto nested_project = installation / "sample";
        std::filesystem::create_directories(nested_project);
        storage::PublicationFileStore nested(nested_project, user, installation);
        const auto nested_target = nested.resolve((nested_project / "source").generic_string());
        assert(nested_target);
        assert(std::holds_alternative<CommitReceipt>(nested.publish({{15}, *nested_target, artifact("nested")})));
        assert(read(nested_project / "source") == "nested");
        std::cout << "Publication roots: project/personal IO, alias, conflict, escape, read-only installation PASS\n";
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
    publicationRoots(root / "roots");
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
    // Extended paths are an IO detail; aliases still resolve to one ordinary physical key.
    std::filesystem::path long_address{"Content"};
    while ((root / long_address).native().size() < 300)
        long_address /= "immutable-generation";
    long_address /= "payload.bin";
    auto long_target = store.resolve(long_address.generic_string());
    assert(long_target && long_target->expected_version == "missing");
    const auto long_artifact = artifact("long published bytes");
    PublicationQuery long_work{{6}, *long_target, long_artifact};
    auto long_outcome = store.publish(long_work);
    assert(std::holds_alternative<CommitReceipt>(long_outcome));
    auto long_read = storage::readPublicationFile(root / long_address, 1024);
    assert(long_read && std::span(*long_read).size() == long_artifact->bytes.size());
    assert(std::ranges::equal(*long_read, long_artifact->bytes.view()));
    auto long_resolved = store.resolve(long_address.generic_string());
    assert(long_resolved && long_resolved->key == long_target->key);
    assert(long_resolved->expected_version == std::get<CommitReceipt>(long_outcome).version);
    assert(std::holds_alternative<NotPublished>(store.publish(long_work)));
    assert(storage::publicationFileDigest(root / long_address) == long_resolved->expected_version);
    std::cout << "Long physical target: read/digest/version conflict retain actual bytes PASS\n";
    std::cout
        << "Actual filesystem access denial preserves old bytes; case alias and unsupported hard-link aliases PASS\n";
    std::cout << "X05-08 real file create/overwrite/conflict/cancel/path-failure/post-replace durability: PASS\n";
    std::cout << "Evidence directory: " << root << '\n';
}
