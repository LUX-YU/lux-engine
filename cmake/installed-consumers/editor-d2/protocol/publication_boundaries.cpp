#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/project/ProjectPublication.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>

using namespace lux::editor;

namespace
{
    lux::asset::AssetId identity(unsigned char value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }
    std::string material(unsigned char id, bool after)
    {
        lux::material::MaterialSourceDocument source{identity(id), after ? "Captured S1" : "Original S0", {}};
        auto encoded = lux::material::encodeMaterialSource(source);
        assert(encoded);
        return std::move(*encoded);
    }
    std::string digest(std::string_view bytes)
    {
        return projectContentDigest(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    }
    auto owned(std::string bytes)
    {
        auto owner = std::make_shared<const std::string>(std::move(bytes));
        return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span(*owner)));
    }
    void write(const std::filesystem::path &path, std::string_view bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(file.good());
    }
    std::string read(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        assert(file.good());
        return {std::istreambuf_iterator<char>{file}, {}};
    }
    ProjectManifest manifest(bool after)
    {
        ProjectManifest result{identity(1), after ? "Captured S1" : "Original S0", {}, {}};
        for (unsigned char id : {2, 3})
        {
            const auto name = std::string(id == 2 ? "A.luxmaterial" : "B.luxmaterial");
            result.assets.push_back(
                {identity(id), EProjectAssetKind::MATERIAL_GRAPH, name, {}, digest(material(id, after)), {}, name});
        }
        return result;
    }
    void inspect(const std::filesystem::path &root, bool first, bool second, bool project)
    {
        for (unsigned char id : {2, 3})
        {
            const auto name = id == 2 ? "A.luxmaterial" : "B.luxmaterial";
            const auto bytes = read(root / name);
            assert(bytes == material(id, id == 2 ? first : second));
            auto decoded = lux::material::decodeMaterialSource(bytes);
            assert(decoded && decoded->id == identity(id));
            std::printf("file=%s sha256=%s bytes=%zu\n", name, digest(bytes).c_str(), bytes.size());
        }
        const auto bytes = read(root / "Project.luxproject");
        assert(bytes == *encodeProjectManifest(manifest(project)));
        assert(decodeProjectManifest(bytes)->id == identity(1));
        std::printf("file=Project.luxproject sha256=%s bytes=%zu\n", digest(bytes).c_str(), bytes.size());
    }
} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 3 || argc == 4);
    std::filesystem::path root{argv[1]};
    const std::string mode{argv[2]};
    if (mode == "ordinary")
    {
        root /= std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    if (mode == "interrupt" || mode == "ordinary")
    {
        assert(!std::filesystem::exists(root));
        std::filesystem::create_directories(root);
        write(root / "A.luxmaterial", material(2, false));
        write(root / "B.luxmaterial", material(3, false));
        write(root / "Project.luxproject", *encodeProjectManifest(manifest(false)));
        auto lease = ProjectWriteLease::acquire(root);
        assert(lease && lease->writable());
        ProjectPublication publication;
        publication.root = root;
        publication.manifest_path = "Project.luxproject";
        publication.before_manifest_digest = *projectFileDigest(root / publication.manifest_path);
        publication.manifest = manifest(true);
        publication.files = {{"A.luxmaterial", digest(material(2, false)), owned(material(2, true))},
                             {"B.luxmaterial", digest(material(3, false)), owned(material(3, true))}};
        auto result = publishProjectFiles(publication);
        if (mode == "interrupt")
        {
            std::puts("FAIL diagnostic boundary was not reached; this is not an interruption result");
            return 4;
        }
        assert(result && result->cleanup && result->published_files == 3);
        inspect(root, true, true, true);
        assert(!std::filesystem::exists(root / ".lux-editor-publication"));
        std::puts("PASS actual source publication: two Material codecs, manifest last, identities and digests");
        return 0;
    }

    assert(argc == 4);
    const std::string boundary{argv[3]};
    const bool committed = boundary.starts_with("committed") || boundary.starts_with("cleanup");
    const bool published = boundary.starts_with("published:");
    const int count = published ? std::stoi(boundary.substr(10)) + 1 : 0;
    auto lease = ProjectWriteLease::acquire(root);
    assert(lease && lease->writable());
    inspect(root, committed || count >= 1, committed || count >= 2, committed || count >= 3);
    if (mode == "conflict")
    {
        const auto other = read(root / "B.luxmaterial");
        const auto project = read(root / "Project.luxproject");
        write(root / "A.luxmaterial", "unknown external bytes; preserve exactly\n");
        auto recovered = recoverProjectFiles(root);
        assert(!recovered);
        const auto *cause = std::any_cast<ProjectPublicationFailure>(&recovered.error().cause);
        assert(cause && cause->code == EProjectPublicationError::RECOVERY_CONFLICT);
        assert(read(root / "A.luxmaterial") == "unknown external bytes; preserve exactly\n");
        assert(read(root / "B.luxmaterial") == other && read(root / "Project.luxproject") == project);
        assert(std::filesystem::exists(root / ".lux-editor-publication/journal.toml"));
        std::puts("PASS exact RECOVERY_CONFLICT: foreign bytes and all other targets retained");
        return 0;
    }
    assert(mode == "recover");
    assert(recoverProjectFiles(root));
    inspect(root, committed, committed, committed);
    assert(!std::filesystem::exists(root / ".lux-editor-publication"));
    std::printf("PASS new-process recovery boundary=%s retained_state=%s\n", boundary.c_str(), committed ? "S1" : "S0");
}
