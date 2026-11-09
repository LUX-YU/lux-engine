#include <cmath>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/ModelImporter.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <nlohmann/json.hpp>

namespace lux::editor
{
    namespace
    {
        constexpr std::size_t source_limit = 256U << 20U;
        constexpr std::size_t package_limit = 512U << 20U;

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

        std::string utf8(const std::filesystem::path& path)
        {
            const auto text = path.generic_u8string();
            return {reinterpret_cast<const char*>(text.data()), text.size()};
        }

        AssetImportResult<AssetImportProduct> encodeModel(toolchain::ModelCookProduct product) noexcept
        {
            std::vector<asset::PakWriteEntry> entries;
            AssetImportProduct result;
            result.root_type = asset::ModelAsset::asset_type;
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
                result.products.push_back({id, Asset::asset_type});
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

    } // namespace

    AssetImportResult<std::vector<std::byte>> ModelImporter::encode(const Configuration& value) noexcept
    {
        const bool invalid_scale = !std::isfinite(value.uniform_scale) || value.uniform_scale <= 0;
        const bool invalid_rotation = !value.pre_rotation.coeffs().allFinite() || value.pre_rotation.norm() < 1e-6F;
        if (invalid_scale || invalid_rotation)
        {
            return cxx::unexpected(failed(EAssetImportError::INVALID_INPUT, "Invalid model import transform"));
        }
        const auto& q = value.pre_rotation;
        const auto text = nlohmann::json{
            {"scale", value.uniform_scale},
            {"rotation", {q.w(), q.x(), q.y(), q.z()}},
            {"left_handed", value.make_left_handed},
            {"animations", value.import_animations}
        }.dump();
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    AssetImportResult<ModelImporter::Configuration> ModelImporter::decode(std::span<const std::byte> bytes) noexcept
    {
        try
        {
            const auto value = nlohmann::json::parse(
                reinterpret_cast<const char*>(bytes.data()),
                reinterpret_cast<const char*>(bytes.data() + bytes.size())
            );
            Configuration result;
            result.uniform_scale = value.at("scale").get<float>();
            const auto q = value.at("rotation").get<std::array<float, 4>>();
            result.pre_rotation = Eigen::Quaternionf{q[0], q[1], q[2], q[3]};
            result.make_left_handed = value.at("left_handed").get<bool>();
            result.import_animations = value.at("animations").get<bool>();
            if (auto encoded = encode(result); !encoded)
            {
                return cxx::unexpected(encoded.error());
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const nlohmann::json::exception&)
        {
            return cxx::unexpected(failed(EAssetImportError::INVALID_INPUT, "Invalid model import configuration"));
        }
    }

    exec::task<AssetImportResult<AssetImportProduct>> ModelImporter::cook(
        execution::ExecutionRuntime& execution,
        AssetImportSource input,
        Configuration configuration,
        execution::TaskReporter reporter
    ) noexcept
    {
        const auto blocking = execution.blockingIoScheduler();
        if (!blocking)
        {
            co_return cxx::unexpected(failed(EAssetImportError::EXECUTION, "Blocking IO is unavailable"));
        }
        const auto stop = reporter.stopToken();
        toolchain::ModelSource source;
        source.entry = utf8(input.file.filename());
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
            auto files = co_await execution::asResult(
                stdexec::schedule(*blocking) |
                stdexec::then(
                    [&]() noexcept -> AssetImportResult<std::vector<toolchain::ModelSourceFile>>
                    {
                        auto read = toolchain::readModelSourceFiles(
                            input.file.parent_path(),
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
            auto attempt = co_await execution::asResult(
                stdexec::schedule(execution.cpuScheduler()) |
                stdexec::then(
                    [&]() noexcept -> AssetImportResult<toolchain::VModelCookAttempt>
                    {
                        auto result =
                            toolchain::cookModel({input.id, asset::ModelAsset::asset_type}, source, configuration);
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
        auto encoded = co_await execution::asResult(
            stdexec::schedule(execution.cpuScheduler()) |
            stdexec::then([product = std::move(product)]() mutable noexcept { return encodeModel(std::move(product)); })
        );
        if (!encoded)
        {
            co_return cxx::unexpected(taskFailure(encoded.error()));
        }
        co_return std::move(*encoded);
    }
} // namespace lux::editor
