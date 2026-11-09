#include <lux/engine/editor/AssetImport.hpp>
#include <lux/engine/platform/FileIo.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <new>
#include <unordered_set>

namespace lux::editor::detail
{
    namespace
    {
        constexpr std::size_t catalog_limit = 16U << 20U;

        AssetImportFailure failed(EAssetImportError code, std::string detail, bool published = false)
        {
            return {code, std::move(detail), published};
        }

        AssetImportFailure taskFailure(const execution::ExecutionFailure<AssetImportFailure>& failure)
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
                auto products = nlohmann::json::array();
                for (const auto& product : record.products)
                {
                    products.push_back({{"id", uuids::to_string(product.id.uuid())}, {"type", product.type.value()}});
                }
                std::vector<std::uint8_t> configuration;
                configuration.reserve(record.configuration.size());
                for (auto byte : record.configuration)
                {
                    configuration.push_back(std::to_integer<std::uint8_t>(byte));
                }
                records.push_back(
                    {{"id", uuids::to_string(record.id.uuid())},
                     {"type", record.type.value()},
                     {"name", record.name},
                     {"source", utf8(record.source)},
                     {"revision", record.revision},
                     {"package", record.package},
                     {"products", std::move(products)},
                     {"importer", record.importer},
                     {"importer_version", record.importer_version},
                     {"configuration", std::move(configuration)}}
                );
            }
            return {
                {"format", "lux.editor.assets"},
                {"version", 2},
                {"revision", catalog.revision},
                {"assets", std::move(records)}
            };
        }

        // The version-1 catalog described only models. Preserve its codec bytes and
        // incomplete dependency types explicitly; migration never decodes a product.
        void migrateVersionOne(nlohmann::json& value)
        {
            auto records = std::move(value.at("models"));
            for (auto& row : records)
            {
                const auto configuration = nlohmann::json{
                    {"scale", row.at("scale")},
                    {"rotation", row.at("rotation")},
                    {"left_handed", row.at("left_handed")},
                    {"animations", row.at("animations")}
                }.dump();
                row["configuration"] = std::vector<std::uint8_t>(configuration.begin(), configuration.end());
                row["importer"] = "lux.import.model";
                row["importer_version"] = 1;
                row["type"] = asset::AssetTypeId::fromName("lux.model").value();
                for (auto& product : row.at("products"))
                {
                    const auto id = product.get<std::string>();
                    product = {
                        {"id", id},
                        {"type", id == row.at("id").get<std::string>() ? row.at("type").get<std::uint64_t>() : 0}
                    };
                }
            }
            value["assets"] = std::move(records);
            value["version"] = 2;
            value.erase("models");
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
                auto value = nlohmann::json::parse(
                    reinterpret_cast<const char*>(bytes->data()),
                    reinterpret_cast<const char*>(bytes->data() + bytes->size())
                );
                if (value.at("format") != "lux.editor.assets")
                {
                    return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Unsupported asset catalog"));
                }
                if (value.at("version") == 1)
                {
                    migrateVersionOne(value);
                }
                const bool invalid_header = value.at("version") != 2 || !value.at("assets").is_array();
                if (invalid_header)
                {
                    return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Unsupported asset catalog"));
                }
                AssetCatalog result;
                result.revision = value.at("revision").get<std::uint64_t>();
                std::unordered_set<asset::AssetId> identities;
                for (const auto& row : value.at("assets"))
                {
                    auto identity = uuids::uuid::from_string(row.at("id").get<std::string>());
                    if (!identity || identity->is_nil())
                    {
                        return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Invalid asset identity"));
                    }
                    ProjectAsset record;
                    record.id = asset::AssetId{*identity};
                    record.type = asset::AssetTypeId{row.at("type").get<std::uint64_t>()};
                    record.name = row.at("name").get<std::string>();
                    record.source = std::filesystem::u8path(row.at("source").get<std::string>());
                    record.revision = row.at("revision").get<std::uint64_t>();
                    record.package = row.at("package").get<std::string>();
                    record.importer = row.at("importer").get<std::string>();
                    record.importer_version = row.at("importer_version").get<std::uint32_t>();
                    const auto path = std::filesystem::u8path(record.package);
                    const bool invalid_path =
                        path.is_absolute() || !record.package.starts_with(".luxassets/products/") ||
                        path.lexically_normal() != path || record.package.find('\\') != std::string::npos ||
                        path.extension() != ".luxpak";
                    const bool invalid_record = invalid_path || record.name.empty() || record.revision == 0 ||
                                                !record.source.is_absolute() || !record.type ||
                                                record.importer.empty() || !record.importer_version ||
                                                !identities.insert(record.id).second;
                    if (invalid_record)
                    {
                        return cxx::unexpected(failed(EAssetImportError::INVALID_CATALOG, "Invalid asset record/path"));
                    }
                    for (auto byte : row.at("configuration").get<std::vector<std::uint8_t>>())
                    {
                        record.configuration.push_back(static_cast<std::byte>(byte));
                    }
                    for (const auto& product : row.at("products"))
                    {
                        auto id = uuids::uuid::from_string(product.at("id").get<std::string>());
                        if (!id || id->is_nil())
                        {
                            return cxx::unexpected(
                                failed(EAssetImportError::INVALID_CATALOG, "Invalid product identity")
                            );
                        }
                        record.products.push_back(
                            {asset::AssetId{*id}, asset::AssetTypeId{product.at("type").get<std::uint64_t>()}}
                        );
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

        AssetImportResult<ImportedAssets> openProducts(
            const std::filesystem::path& root,
            AssetCatalog catalog,
            std::stop_token stop
        ) noexcept
        {
            ImportedAssets result;
            result.catalog = std::move(catalog);
            result.complete_catalog = true;
            for (const auto& record : result.catalog.records)
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
                result.roots.push_back(record.id);
                result.mounts.push_back({"/Game", std::move(*provider), 0});
            }
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
        const auto blocking = execution.blockingIoScheduler();
        if (!blocking)
        {
            co_return cxx::unexpected(failed(EAssetImportError::EXECUTION, "Blocking IO is unavailable"));
        }
        const auto stop = reporter.stopToken();
        reporter.setPhase("Read asset catalog");
        auto files = co_await execution::asResult(
            stdexec::schedule(*blocking) | stdexec::then(
                                               [&]() noexcept -> AssetImportResult<ImportedAssets>
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
        co_return std::move(*files);
    }

    exec::task<AssetImportResult<ImportedAssets>> importAsset(
        execution::ExecutionRuntime& execution,
        std::filesystem::path root,
        AssetCatalog expected,
        ProjectAsset candidate,
        AssetImportCook cook,
        execution::TaskReporter reporter
    ) noexcept
    {
        const auto blocking = execution.blockingIoScheduler();
        if (!blocking)
        {
            co_return cxx::unexpected(failed(EAssetImportError::EXECUTION, "Blocking IO is unavailable"));
        }
        const auto stop = reporter.stopToken();
        auto encoded = co_await cook(execution, {candidate.id, candidate.source}, candidate.configuration, reporter);
        if (!encoded)
        {
            co_return cxx::unexpected(encoded.error());
        }
        const auto root_product = std::ranges::find(encoded->products, candidate.id, &AssetProduct::id);
        const bool invalid_root = !encoded->root_type || root_product == encoded->products.end() ||
                                  (root_product != encoded->products.end() && root_product->type != encoded->root_type);
        if (invalid_root || encoded->package.empty())
        {
            co_return cxx::unexpected(failed(EAssetImportError::ENCODE, "Importer did not produce its declared root"));
        }
        std::unordered_set<asset::AssetId> product_ids;
        for (const auto& product : encoded->products)
        {
            const bool invalid_product = product.id.isNull() || !product.type || !product_ids.insert(product.id).second;
            if (invalid_product)
            {
                co_return cxx::unexpected(failed(EAssetImportError::ENCODE, "Invalid or duplicate product identity"));
            }
        }
        candidate.type = encoded->root_type;
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
        auto published = co_await execution::asResult(
            stdexec::schedule(*blocking) |
            stdexec::then([&]() noexcept { return publish(root, expected, next, candidate, encoded->package, stop); })
        );
        if (!published)
        {
            co_return cxx::unexpected(taskFailure(published.error()));
        }
        // Publishing succeeded. Complete adoption without a late stop replacing that fact.
        auto provider = co_await execution::asResult(
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
        result.roots.push_back(candidate.id);
        reporter.setProgress(1, 1);
        co_return result;
    }
} // namespace lux::editor::detail
