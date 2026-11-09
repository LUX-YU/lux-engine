#include <lux/cxx/core/scope_exit.hpp>
#include "support/ModelSource.hpp"
#include "support/PreparedProjectFixture.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/AssetImportEditor.hpp>
#include <lux/engine/editor/AssetImportValues.value_editor.hpp>
#include <lux/engine/editor/ModelImporter.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/platform/FileIo.hpp>
#include <lux/engine/process/assets/AssetRepository.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <nlohmann/json.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;

struct TriangleImporter final
{
    using Configuration = fixture::TriangleImportConfiguration;
    static constexpr std::string_view name = "test.import.triangle";
    static constexpr std::uint32_t version = 1;

    static AssetImportResult<std::vector<std::byte>> encode(const Configuration& value) noexcept
    {
        if (value.scale < 1 || value.scale > 32)
        {
            return cxx::unexpected(AssetImportFailure{EAssetImportError::INVALID_INPUT, "Invalid triangle scale"});
        }
        std::vector<std::byte> result;
        for (unsigned i = 0; i < 4; ++i)
        {
            result.push_back(static_cast<std::byte>((value.scale >> (i * 8)) & 255));
        }
        return result;
    }

    static AssetImportResult<Configuration> decode(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() != 4)
        {
            return cxx::unexpected(
                AssetImportFailure{EAssetImportError::INVALID_INPUT, "Invalid triangle configuration"}
            );
        }
        Configuration value{0};
        for (unsigned i = 0; i < 4; ++i)
        {
            value.scale |= std::to_integer<std::uint32_t>(bytes[i]) << (i * 8);
        }
        if (!encode(value))
        {
            return cxx::unexpected(AssetImportFailure{EAssetImportError::INVALID_INPUT, "Invalid triangle scale"});
        }
        return value;
    }

    static AssetImportResult<AssetImportProduct> build(asset::AssetId id, Configuration configuration) noexcept
    {
        auto mesh = std::make_shared<rdesc::Mesh>();
        mesh->vertices.resize(3);
        mesh->indices = {0, 1, 2};
        const float scale = static_cast<float>(configuration.scale);
        mesh->vertices[0].position = {-scale, -scale, 0};
        mesh->vertices[1].position = {scale, -scale, 0};
        mesh->vertices[2].position = {0, scale, 0};
        for (auto& vertex : mesh->vertices)
        {
            vertex.normal = {0, 0, 1};
            vertex.tangent = {1, 0, 0};
            vertex.bitangent = {0, 1, 0};
        }
        mesh->bounds = math::AABB{{-scale, -scale, 0}, {scale, scale, 0}};
        auto created = asset::MeshAsset::create({id, asset::MeshAsset::asset_type}, mesh);
        assert(created);
        auto encoded = asset::TAssetSerDeser<asset::MeshAsset>::encode(**created, asset::AssetEncodeLimits{1U << 20U});
        assert(encoded);
        auto bytes = cxx::SharedBytes<>::copyOf(std::span<const std::byte>(*encoded));
        auto package = asset::encodePak({{id, asset::MeshAsset::primary_magic, "triangle", {}, bytes}}, 1U << 20U);
        assert(package);
        return AssetImportProduct{
            asset::MeshAsset::asset_type,
            {{id, asset::MeshAsset::asset_type}},
            std::move(*package)
        };
    }

    static exec::task<AssetImportResult<AssetImportProduct>> cook(
        execution::ExecutionRuntime& execution,
        AssetImportSource input,
        Configuration configuration,
        execution::TaskReporter reporter
    ) noexcept
    {
        auto blocking = execution.blockingIoScheduler();
        assert(blocking);
        auto source = co_await execution::asResult(
            stdexec::schedule(*blocking) |
            stdexec::then([&]() noexcept { return platform::readFileBounded(input.file, 32, reporter.stopToken()); })
        );
        if (!source)
        {
            co_return cxx::unexpected(AssetImportFailure{EAssetImportError::READ, "Triangle source unavailable"});
        }
        auto product = co_await execution::asResult(
            stdexec::schedule(execution.cpuScheduler()) |
            stdexec::then([&]() noexcept { return build(input.id, configuration); })
        );
        assert(product);
        co_return std::move(*product);
    }
};

int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    auto engine = engine::Engine::create({2, 64, {16}, execution::BlockingSchedulerConfig{1, 16}}, 64);
    assert(engine);
    cxx::scope_exit close_engine([&]() noexcept { assert((*engine)->close()); });
    const auto project_id = *uuids::uuid::from_string("889c1234-0000-0000-0000-111111111111");
    auto context = fixture::createContext(
        **engine,
        {"Importers", root},
        {1, project_id, "Importers"},
        [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; }
    );
    assert(context);
    auto model_importer = declareAssetImporter<ModelImporter>();
    auto triangle_importer = declareAssetImporter<TriangleImporter>();
    assert(model_importer && triangle_importer);
    auto form = declareAssetImportEditor<TriangleImporter>();
    auto draft = form.create(triangle_importer->default_configuration);
    assert(draft);
    auto form_bytes = form.encode(**draft);
    assert(form_bytes && *form_bytes == *TriangleImporter::encode({1}));
    const auto file = root / ".luxassets" / "catalog.json";
    // Reuse the existing fixture directory without accumulating old product trees.
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    ProjectAssets assets(**context, {std::move(*model_importer), std::move(*triangle_importer)});
    const auto wait = [&]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (assets.busy())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert((*engine)->execution().dispatchMainCompletions());
            assert((*engine)->execution().dispatchTaskEvents());
            std::this_thread::yield();
        }
        if (assets.failure())
        {
            std::fprintf(stderr, "%s\n", assets.failure()->detail.c_str());
        }
        assert(!assets.failure());
    };
    assert(assets.open());
    wait();
    assert(
        assets.importAsset(ModelImporter::name, 1, fixture::modelSource(root / "Sources"), *ModelImporter::encode({}))
    );
    wait();
    const auto imported = assets.records().front();
    nlohmann::json catalog;
    {
        std::ifstream input(file);
        input >> catalog;
    }
    auto record = catalog.at("assets").front();
    auto configuration = nlohmann::json::parse(
        std::string(reinterpret_cast<const char*>(imported.configuration.data()), imported.configuration.size())
    );
    for (auto i = configuration.begin(); i != configuration.end(); ++i)
    {
        record[i.key()] = i.value();
    }
    for (auto& product : record["products"])
    {
        product = product.at("id").get<std::string>();
    }
    catalog["version"] = 1;
    catalog["models"] = nlohmann::json::array({record});
    catalog.erase("assets");
    {
        std::ofstream output(file);
        output << catalog;
    }
    assert(assets.open());
    wait();
    assert(
        assets.records().front().id == imported.id && assets.records().front().configuration == imported.configuration
    );
    assert(assets.reimport(imported.id));
    wait();
    {
        std::ifstream input(file);
        input >> catalog;
    }
    auto& unknown = catalog["assets"][0];
    unknown["importer"] = "unavailable.provider";
    unknown["importer_version"] = 99;
    unknown["configuration"] = {0, 255, 7, 0};
    const auto preserved = unknown;
    {
        std::ofstream output(file);
        output << catalog;
    }
    assert(assets.open());
    wait();
    auto rejected = assets.reimport(imported.id);
    assert(!rejected && rejected.error().code == EAssetImportError::UNKNOWN_IMPORTER);
    const auto source = root / "triangle.source";
    fixture::writeText(source, "triangle");
    assert(assets.importAsset(TriangleImporter::name, 1, source, *TriangleImporter::encode({3})));
    wait();
    assert(assets.records().size() == 2 && assets.records().back().type == asset::MeshAsset::asset_type);
    {
        std::ifstream input(file);
        input >> catalog;
    }
    assert(catalog.at("assets")[0] == preserved);
    process::assets::AssetDataRef<asset::MeshAsset::data_type> mesh;
    auto use = (*(*context)->prepareAssets())->acquire<asset::MeshAsset>(assets.records().back().id);
    assert(use);
    auto load = (*(*context)->prepareAssets())->read(*use);
    assert((*engine)->execution().spawn(stdexec::then(
        stdexec::continues_on(
            execution::asResult(std::move(load)),
            (*engine)->execution().mainScheduler()
        ),
        [&](auto result) noexcept
        {
            assert(result);
            mesh = std::move(*result);
        }
    )));
    while (!mesh)
    {
        assert((*engine)->execution().dispatchMainCompletions());
    }
    assert(mesh->vertices.size() == 3 && mesh->vertices[0].position.x() == -3.0F);
    std::puts("PASS Editor import declarations: Model/multiple products and Mesh root, generated typed form, portable "
              "codec, v1 migration, unknown bytes preserved, on-demand decode");
}
