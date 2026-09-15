#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/project/ProjectPublication.hpp>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

using namespace lux::editor;

void textFile(const std::filesystem::path &path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), bytes.size());
    assert(file.good());
}
std::string readText(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
auto owned(std::string text)
{
    auto bytes = std::make_shared<const std::string>(std::move(text));
    return lux::cxx::SharedBytes<>::fromOwner(bytes, std::as_bytes(std::span(*bytes)));
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 3);
    const std::filesystem::path root{argv[1]};
    const std::string mode{argv[2]};
    if (mode == "recover")
    {
        auto lease = ProjectWriteLease::acquire(root);
        assert(lease && lease->writable());
        assert(readText(root / "Notes.txt") == "captured S1");
        auto recovered = recoverProjectFiles(root);
        assert(recovered);
        assert(readText(root / "Notes.txt") == "original S0");
        assert(!std::filesystem::exists(root / ".lux-editor-publication"));
        std::puts("PASS recovery-after-deliberate-interruption: original bytes restored, kernel lease reacquired");
        return 0;
    }
    assert(!std::filesystem::exists(root));
    std::filesystem::create_directories(root);
    auto lease = ProjectWriteLease::acquire(root);
    assert(lease && lease->writable());
    auto second = ProjectWriteLease::acquire(root);
    assert(second && !second->writable());
    const auto id = *uuids::uuid::from_string("e9c15271-c836-464c-af66-c8209492de8b");
    ProjectManifest manifest{lux::asset::AssetId{id}, "Original", {}, {}};
    auto encoded = encodeProjectManifest(manifest);
    assert(encoded);
    textFile(root / "Project.luxproject", *encoded);
    textFile(root / "Notes.txt", "original S0");
    ProjectPublication publication;
    publication.root = root;
    publication.manifest_path = "Project.luxproject";
    publication.before_manifest_digest = *projectFileDigest(root / publication.manifest_path);
    publication.manifest = manifest;
    publication.manifest.name = "Saved";
    publication.files.push_back({"Notes.txt", *projectFileDigest(root / "Notes.txt"), owned("captured S1")});
    if (mode == "cancel")
    {
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = publishProjectFiles(publication, stop.get_token());
        assert(!cancelled);
        assert(std::any_cast<ProjectPublicationFailure>(cancelled.error().cause).code ==
               EProjectPublicationError::CANCELLED);
        assert(readText(root / "Notes.txt") == "original S0");
        assert(recoverProjectFiles(root));
        std::puts("PASS publication-cancel: no source effects, PREPARING journal reclaimed");
        return 0;
    }
    const auto denied =
        CreateFileW((root / "Project.luxproject").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(denied != INVALID_HANDLE_VALUE);
    auto failed = publishProjectFiles(publication);
    assert(!failed);
    const auto cause = std::any_cast<ProjectPublicationFailure>(failed.error().cause);
    assert(cause.code == EProjectPublicationError::REPLACE && cause.published_files == 1);
    assert(readText(root / "Notes.txt") == "captured S1");
    assert(readText(root / "Project.luxproject") == *encoded);
    std::puts("observed PREPARED partial publication: source=S1, manifest=S0, exact REPLACE error, effects=1");
    if (mode == "crash")
    {
        std::puts("DELIBERATE INTERRUPTION exit=86: recovery test, not normal Editor shutdown");
        std::_Exit(86);
    }
    CloseHandle(denied);
    if (mode == "conflict")
    {
        textFile(root / "Notes.txt", "external content");
        auto rejected = recoverProjectFiles(root);
        assert(!rejected);
        assert(std::any_cast<ProjectPublicationFailure>(rejected.error().cause).code ==
               EProjectPublicationError::RECOVERY_CONFLICT);
        assert(readText(root / "Notes.txt") == "external content");
        assert(std::filesystem::exists(root / ".lux-editor-publication/journal.toml"));
        std::puts("PASS recovery-conflict: external bytes retained and publication ownership evidence retained");
        return 0;
    }
    assert(recoverProjectFiles(root));
    assert(readText(root / "Notes.txt") == "original S0");
    assert(!std::filesystem::exists(root / ".lux-editor-publication"));
    auto saved = publishProjectFiles(publication);
    assert(saved && saved->cleanup && saved->published_files == 2);
    assert(readText(root / "Notes.txt") == "captured S1");
    assert(decodeProjectManifest(readText(root / "Project.luxproject"))->name == "Saved");
    std::puts("PASS publication-recovery-retry: rollback S0, republish S1, manifest last, exact content verified");
}
