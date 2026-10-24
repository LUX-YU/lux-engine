#include <lux/engine/editor/AssetImport.hpp>
#include <lux/engine/platform/FileIo.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <new>

namespace lux::editor::detail
{
    namespace
    {
        constexpr std::size_t catalog_limit = 16U << 20U;
        constexpr std::size_t source_limit = 256U << 20U;
        constexpr std::size_t package_limit = 512U << 20U;

        AssetImportFailure failed(EAssetImportError code, std::string detail, bool published = false)
        {
            return {code, std::move(detail), published};
        }

        AssetImportFailure taskFailure(const execution::TTaskError<AssetImportFailure>& failure)
        {
            if (const auto* domain = failure.domainFailure())
            {
                return *domain;
            }
            return failed(
                failure.isCancelled() ? EAssetImportError::CANCELLED : EAssetImportError::EXECUTION,
                "Asset operation could not execute"
            );
        }

        std::filesystem::path catalogPath(const std::filesystem::path& root)
        {
            return root / ".luxassets" / "catalog.json";
        }

        std::string utf8(const std::filesystem::path& path)
        {
            const auto text = path.generic_u8string();
            return {reinterpret_cast<const char*>(text.data()), text.size()};
        }

        nlohmann::json catalogJson(const AssetCatalog& catalog)
        {
            auto records = nlohmann::json::array();
            for (const auto& record : catalog.records)
            {
                const auto& settings = record.configuration;
                const auto& rotation = settings.pre_rotation;
                auto products = nlohmann::json::array();
                for (auto id : record.products)
                {
                    products.push_back(uuids::to_string(id.uuid()));
                }
                records.push_back(
                    {{"id", uuids::to_string(record.id.uuid())},
                     {"name", record.name},
                     {"source", utf8(record.source)},
                     {"revision", record.revision},
                     {"package", record.package},
                     {"products", std::move(products)},
                     {"scale", settings.uniform_scale},
                     {"rotation", {rotation.w(), rotation.x(), rotation.y(), rotation.z()}},
                     {"left_handed", settings.make_left_handed},
                     {"animations", settings.import_animations}}
                );
            }
            return {
                {"format", "lux.editor.assets"},
                {"version", 1},
                {"revision", catalog.revision},
                {"models", std::move(records)}
            };
        }

        AssetImportResult<AssetCatalog> readCatalog(const std::filesystem::path& root, std::stop_token stop) noexcept
        {
            auto bytes = platform::readFileBounded(catalogPath(root), catalog_limit, stop);
            if (!bytes)
            {
                if (bytes.error().system == std::errc::no_such_file_or_directory)
                {
                    return AssetCatalog{};
                }
                return cxx::unexpected(failed(
                    bytes.error().code == platform::EFileIoError::CANCELLED ? EAssetImportError::CANCELLED
                                                                            : EAssetImportError::READ,
                    "Asset catalog: " + bytes.error().system.message()
                ));
            }
            try
            {
                const auto value = nlohmann::json::parse(
                    reinterpret_cast<const char*>(bytes->data()),
                    reinterpret_cast<const char*>(bytes->data() + bytes->size())
                );
                const bool invalid_header = value.at("format") != "lux.editor.assets" || value.at("version") != 1 ||
                                            !value.at("models").is_array();
                if (invalid_header)
                {
                    return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Unsupported asset catalog"));
                }
                AssetCatalog result;
                result.revision = value.at("revision").get<std::uint64_t>();
                std::vector<asset::AssetId> identities;
                for (const auto& row : value.at("models"))
                {
                    auto identity = uuids::uuid::from_string(row.at("id").get<std::string>());
                    if (!identity || identity->is_nil())
                    {
                        return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Invalid model identity"));
                    }
                    ProjectAsset record;
                    record.id = asset::AssetId{*identity};
                    record.name = row.at("name").get<std::string>();
                    record.source = std::filesystem::u8path(row.at("source").get<std::string>());
                    record.revision = row.at("revision").get<std::uint64_t>();
                    record.package = row.at("package").get<std::string>();
                    const auto path = std::filesystem::u8path(record.package);
                    const bool invalid_path =
                        path.is_absolute() || !record.package.starts_with(".luxassets/products/") ||
                        path.lexically_normal() != path || record.package.find('\\') != std::string::npos ||
                        path.extension() != ".luxpak";
                    const bool invalid_record = invalid_path || record.name.empty() || record.revision == 0 ||
                                                !record.source.is_absolute() ||
                                                std::ranges::find(identities, record.id) != identities.end();
                    if (invalid_record)
                    {
                        return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Invalid asset record/path"));
                    }
                    identities.push_back(record.id);
                    auto& settings = record.configuration;
                    settings.uniform_scale = row.at("scale").get<float>();
                    const auto rotation = row.at("rotation").get<std::array<float, 4>>();
                    settings.pre_rotation = Eigen::Quaternionf{rotation[0], rotation[1], rotation[2], rotation[3]};
                    settings.make_left_handed = row.at("left_handed").get<bool>();
                    settings.import_animations = row.at("animations").get<bool>();
                    for (const auto& product : row.at("products"))
                    {
                        auto id = uuids::uuid::from_string(product.get<std::string>());
                        if (!id || id->is_nil())
                        {
                            return cxx::unexpected(
                                failed(EAssetImportError::INVALID_CATALOG, "Invalid product identity")
                            );
                        }
                        record.products.push_back(asset::AssetId{*id});
                    }
                    result.records.push_back(std::move(record));
                }
                return result;
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (const nlohmann::json::exception&)
            {
                return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Malformed asset catalog"));
            }
        }

        struct CatalogFiles final
        {
            ImportedAssets values;
            std::vector<asset::AssetBlob> models;
        };

        AssetImportResult<CatalogFiles> openProducts(
            std::filesystem::path root,
            AssetCatalog catalog,
            std::stop_token stop
        ) noexcept
        {
            CatalogFiles result;
            result.values.catalog = std::move(catalog);
            result.values.complete_catalog = true;
            for (const auto& record : result.values.catalog.records)
            {
                if (stop.stop_requested())
                {
                    return cxx::unexpected(failed(EAssetImportError::CANCELLED, "Asset opening cancelled"));
                }
                auto provider = asset::PakAssetProvider::loadFromFile(root / std::filesystem::u8path(record.package));
                if (!provider)
                {
                    return cxx::unexpected(failed(EAssetImportError::READ, provider.error()));
                }
                auto model = (*provider)->open(record.id, source_limit);
                if (!model)
                {
                    return cxx::unexpected(
                        failed(EAssetImportError::READ, "Model root is missing from its product package")
                    );
                }
                result.models.push_back(std::move(*model));
                result.values.mounts.push_back({"/Game", std::move(*provider), 0});
            }
            return result;
        }

        AssetImportResult<ImportedAssets> decodeModels(CatalogFiles files) noexcept
        {
            for (std::size_t i = 0; i < files.models.size(); ++i)
            {
                auto model = asset::TAssetSerDeser<asset::ModelAsset>::decode(
                    files.values.catalog.records[i].id,
                    std::move(files.models[i].bytes),
                    {source_limit, source_limit, 32}
                );
                if (!model)
                {
                    return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Invalid Cooked model root"));
                }
                files.values.models.push_back(std::move(*model));
            }
            return std::move(files.values);
        }

        struct EncodedModel final
        {
            std::vector<std::byte> package;
            std::vector<asset::AssetId> products;
            std::shared_ptr<const asset::ModelAsset> model;
        };

        AssetImportResult<EncodedModel> encodeModel(toolchain::ModelCookProduct product) noexcept
        {
            std::vector<asset::PakWriteEntry> entries;
            EncodedModel result;
            result.model = product.model;
            const auto append = [&]<class Asset>(std::shared_ptr<const Asset>& asset) -> bool
            {
                auto encoded = asset::TAssetSerDeser<Asset>::encode(*asset, asset::AssetEncodeLimits{package_limit});
                if (!encoded)
                {
                    return false;
                }
                const auto id = asset->id();
                auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
                entries.push_back(
                    {id,
                     Asset::primary_magic,
                     uuids::to_string(id.uuid()),
                     {},
                     cxx::SharedBytes<>::fromOwner(owner, *owner)}
                );
                result.products.push_back(id);
                asset.reset();
                return true;
            };
            if (!append(product.model))
            {
                return cxx::unexpected(failed(EAssetImportError::ENCODE, "Model encoding failed"));
            }
            const auto append_all = [&](auto& values)
            { return std::ranges::all_of(values, [&](auto& value) { return append(value); }); };
            const bool encoded = append_all(product.meshes) && append_all(product.materials) &&
                                 append_all(product.textures) && (!product.skeleton || append(*product.skeleton)) &&
                                 append_all(product.animations);
            if (!encoded)
            {
                return cxx::unexpected(failed(EAssetImportError::ENCODE, "Model dependency encoding failed"));
            }
            auto package = asset::encodePak(entries, package_limit);
            if (!package)
            {
                return cxx::unexpected(failed(EAssetImportError::ENCODE, package.error()));
            }
            result.package = std::move(*package);
            return result;
        }

        AssetImportResult<void> publish(
            const std::filesystem::path& root,
            const AssetCatalog& expected,
            const AssetCatalog& candidate,
            const ProjectAsset& record,
            std::span<const std::byte> package,
            std::stop_token stop
        ) noexcept
        {
            auto current = readCatalog(root, stop);
            if (!current)
            {
                return cxx::unexpected(current.error());
            }
            if (catalogJson(*current) != catalogJson(expected))
            {
                return cxx::unexpected(failed(EAssetImportError::STALE_CATALOG, "Asset catalog changed during import"));
            }
            const auto file = root / std::filesystem::u8path(record.package);
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec)
            {
                return cxx::unexpected(failed(EAssetImportError::PUBLICATION, ec.message()));
            }
            auto written = platform::writeFileAtomic(file, package, platform::EFileWriteMode::CREATE, stop);
            if (!written)
            {
                return cxx::unexpected(
                    failed(EAssetImportError::PUBLICATION, "Product write: " + written.error().system.message())
                );
            }
            const auto json = catalogJson(candidate).dump(2);
            auto replaced = platform::writeFileAtomic(
                catalogPath(root),
                std::as_bytes(std::span(json.data(), json.size())),
                expected.revision ? platform::EFileWriteMode::REPLACE : platform::EFileWriteMode::CREATE,
                {}
            );
            if (!replaced)
            {
                const bool uncertain = replaced.error().code == platform::EFileIoError::PUBLICATION_UNKNOWN;
                if (!uncertain)
                {
                    std::filesystem::remove(file, ec); // Only this operation's unpublished candidate.
                }
                return cxx::unexpected(failed(
                    EAssetImportError::PUBLICATION,
                    "Catalog write: " + replaced.error().system.message(),
                    uncertain
                ));
            }
            return {};
        }
    } // namespace

    exec::task<AssetImportResult<ImportedAssets>> openAssetCatalog(
        execution::ExecutionRuntime& execution,
        std::filesystem::path root,
        execution::TaskReporter reporter
    ) noexcept
    {
        const auto blocking = execution.blocking();
        if (!blocking)
        {
            co_return cxx::unexpected(failed(EAssetImportError::EXECUTION, "Blocking IO is unavailable"));
        }
        const auto stop = reporter.stopToken();
        reporter.setPhase("Read asset catalog");
        auto files = co_await execution::taskResult(
            stdexec::schedule(*blocking) | stdexec::then(
                                               [&]() noexcept -> AssetImportResult<CatalogFiles>
                                               {
                                                   auto catalog = readCatalog(root, stop);
                                                   if (!catalog)
                                                   {
                                                       return cxx::unexpected(catalog.error());
                                                   }
                                                   return openProducts(root, std::move(*catalog), stop);
                                               }
                                           )
        );
        if (!files)
        {
            co_return cxx::unexpected(taskFailure(files.error()));
        }
        reporter.setPhase("Decode model roots");
        auto models = co_await execution::taskResult(
            stdexec::schedule(execution.cpu()) |
            stdexec::then([files = std::move(*files)]() mutable noexcept { return decodeModels(std::move(files)); })
        );
        if (!models)
        {
            co_return cxx::unexpected(taskFailure(models.error()));
        }
        co_return std::move(*models);
    }

    exec::task<AssetImportResult<ImportedAssets>> importModel(
        execution::ExecutionRuntime& execution,
        std::filesystem::path root,
        AssetCatalog expected,
        ProjectAsset candidate,
        execution::TaskReporter reporter
    ) noexcept
    {
        const auto blocking = execution.blocking();
        if (!blocking)
        {
            co_return cxx::unexpected(failed(EAssetImportError::EXECUTION, "Blocking IO is unavailable"));
        }
        const auto stop = reporter.stopToken();
        toolchain::ModelSource source;
        source.entry = utf8(candidate.source.filename());
        std::vector<std::string> requested{source.entry};
        toolchain::ModelCookProduct product;
        std::size_t source_bytes{};
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(failed(EAssetImportError::CANCELLED, "Import cancelled before publication"));
            }
            reporter.setPhase("Read model dependencies");
            auto files = co_await execution::taskResult(
                stdexec::schedule(*blocking) |
                stdexec::then(
                    [&]() noexcept -> AssetImportResult<std::vector<toolchain::ModelSourceFile>>
                    {
                        auto read = toolchain::readModelSourceFiles(
                            candidate.source.parent_path(),
                            requested,
                            source_limit - source_bytes
                        );
                        if (!read)
                        {
                            return cxx::unexpected(failed(EAssetImportError::READ, read.error().detail));
                        }
                        return std::move(*read);
                    }
                )
            );
            if (!files)
            {
                co_return cxx::unexpected(taskFailure(files.error()));
            }
            for (auto& file : *files)
            {
                source_bytes += file.bytes.size();
                source.files.push_back(std::move(file));
            }
            reporter.setPhase("Cook model, materials and textures");
            auto attempt = co_await execution::taskResult(
                stdexec::schedule(execution.cpu()) |
                stdexec::then(
                    [&]() noexcept -> AssetImportResult<toolchain::VModelCookAttempt>
                    {
                        auto result = toolchain::cookModel(
                            {candidate.id, asset::ModelAsset::asset_type},
                            source,
                            candidate.configuration
                        );
                        if (const auto* failure = std::get_if<toolchain::ModelCookFailure>(&result))
                        {
                            return cxx::unexpected(failed(EAssetImportError::COOK, failure->detail));
                        }
                        return result;
                    }
                )
            );
            if (!attempt)
            {
                co_return cxx::unexpected(taskFailure(attempt.error()));
            }
            if (auto* requests = std::get_if<toolchain::ModelSourceRequests>(&*attempt))
            {
                requested = std::move(requests->paths);
                continue;
            }
            product = std::move(std::get<toolchain::ModelCookProduct>(*attempt));
            break;
        }
        source = {}; // Cooked objects no longer borrow captured files.
        reporter.setPhase("Encode immutable products");
        auto encoded = co_await execution::taskResult(
            stdexec::schedule(execution.cpu()) |
            stdexec::then([product = std::move(product)]() mutable noexcept { return encodeModel(std::move(product)); })
        );
        if (!encoded)
        {
            co_return cxx::unexpected(taskFailure(encoded.error()));
        }
        candidate.products = std::move(encoded->products);
        AssetCatalog next = expected;
        ++next.revision;
        const auto previous = std::ranges::find(next.records, candidate.id, &ProjectAsset::id);
        if (previous == next.records.end())
        {
            next.records.push_back(candidate);
        }
        else
        {
            *previous = candidate;
        }
        reporter.setPhase("Publish asset catalog");
        auto published = co_await execution::taskResult(
            stdexec::schedule(*blocking) |
            stdexec::then([&]() noexcept { return publish(root, expected, next, candidate, encoded->package, stop); })
        );
        if (!published)
        {
            co_return cxx::unexpected(taskFailure(published.error()));
        }
        // Publishing succeeded. Complete adoption without a late stop replacing that fact.
        auto provider = co_await execution::taskResult(
            stdexec::schedule(*blocking) |
            stdexec::then(
                [&]() noexcept -> AssetImportResult<std::shared_ptr<asset::PakAssetProvider>>
                {
                    auto loaded =
                        asset::PakAssetProvider::loadFromFile(root / std::filesystem::u8path(candidate.package));
                    if (!loaded)
                    {
                        return cxx::unexpected(failed(EAssetImportError::READ, loaded.error(), true));
                    }
                    return std::move(*loaded);
                }
            )
        );
        if (!provider)
        {
            auto error = taskFailure(provider.error());
            error.catalog_published = true;
            co_return cxx::unexpected(std::move(error));
        }
        ImportedAssets result;
        result.catalog = std::move(next);
        result.mounts.push_back({"/Game", std::move(*provider), 0});
        result.models.push_back(std::move(encoded->model));
        reporter.setProgress(1, 1);
        co_return result;
    }
} // namespace lux::editor::detail
