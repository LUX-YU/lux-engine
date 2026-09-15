#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/editor/detail/ProjectWrite.hpp>
#include <lux/engine/editor/asset/AssetImporter.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>

namespace lux::editor::assets
{
namespace
{
constexpr std::size_t source_limit = 256U * 1024U * 1024U;
auto failed(EEditorError code, std::string domain, std::string message = {})
{
    return lux::cxx::unexpected(EditorFailure{code, std::move(domain), 0, std::move(message)});
}
auto cookFailed(lux::toolchain::ModelCookFailure error)
{
    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "model.cook",
                                              static_cast<std::uint64_t>(error.code), error.detail, std::move(error)});
}
lux::cxx::SharedBytes<> own(std::vector<std::byte> bytes)
{
    auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
}
lux::cxx::SharedBytes<> own(std::string text)
{
    auto owner = std::make_shared<const std::string>(std::move(text));
    return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span(*owner)));
}
struct Source final
{
    std::filesystem::path root;
    lux::toolchain::ModelSource capture;
    lux::toolchain::ModelCookConfiguration config;
};
struct Load final
{
    std::filesystem::path file;
    lux::toolchain::ModelCookConfiguration config;
    bool recipe{};
    std::filesystem::path replacement;
    std::string expected_digest;
    EditorResult<Source> operator()() const noexcept
    {
        if (!recipe)
        {
            return Source{file.parent_path(), {file.filename().generic_string(), {}}, config};
        }
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        const auto size = stream.tellg();
        if (!stream || size < 0 || size > 16U * 1024U * 1024U)
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.read", file.string());
        }
        std::string bytes(static_cast<std::size_t>(size), '\0');
        stream.seekg(0);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream || stream.peek() != std::char_traits<char>::eof() || stream.bad())
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.read", file.string());
        }
        if (projectContentDigest(std::as_bytes(std::span(bytes))) != expected_digest)
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.conflict", file.string());
        }
        auto parsed = toml::parse(bytes);
        if (!parsed)
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.parse",
                          std::string(parsed.error().description()));
        }
        auto &table = parsed.table();
        constexpr std::array known_fields{"format",      "version",    "root",     "entry", "scale",
                                          "left_handed", "animations", "rotation", "files"};
        for (const auto &[key, value] : table)
        {
            if (std::ranges::find(known_fields, key.str()) == known_fields.end())
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.unknown-field", std::string(key.str()));
            }
        }
        const auto format = table["format"].value<std::string>();
        const auto version = table["version"].value<std::int64_t>();
        const auto root = table["root"].value<std::string>();
        const auto entry = table["entry"].value<std::string>();
        const auto scale = table["scale"].value<double>();
        const auto handed = table["left_handed"].value<bool>();
        const auto animated = table["animations"].value<bool>();
        const auto *rotation = table["rotation"].as_array();
        const auto *files = table["files"].as_array();
        if (!format || *format != "lux.editor.model-source" || !version || *version != 1 || !root || !entry ||
            !validProjectPath(*root) || !validProjectPath(*entry) || !scale || !handed || !animated || !rotation ||
            rotation->size() != 4 || !files || files->size() > 4096)
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.schema");
        }
        if (!std::isfinite(*scale) || *scale <= 0.0 || *scale > std::numeric_limits<float>::max())
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.scale");
        }
        std::vector<std::string> paths, digests;
        std::set<std::string> unique_paths;
        for (const auto &item : *files)
        {
            const auto *record = item.as_table();
            if (!record || record->size() != 2)
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.files");
            }
            const auto path = (*record)["path"].value<std::string>();
            const auto digest = (*record)["digest"].value<std::string>();
            if (!path || !digest || !validProjectPath(*path) || digest->size() != 64 ||
                !std::ranges::all_of(*digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.files");
            }
            auto folded = *path;
            for (auto &character : folded)
            {
                if (character >= 'A' && character <= 'Z')
                {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if (!unique_paths.insert(folded).second)
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.duplicate-path", *path);
            }
            paths.push_back(*path);
            digests.push_back(*digest);
        }
        if (std::ranges::find(paths, *entry) == paths.end())
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.entry", *entry);
        }
        Source result{file.parent_path() / *root, {*entry, {}}, {}};
        result.config.uniform_scale = static_cast<float>(*scale);
        result.config.make_left_handed = *handed;
        result.config.import_animations = *animated;
        for (std::size_t index{}; index < 4; ++index)
        {
            const auto value = (*rotation)[index].value<double>();
            if (!value || !std::isfinite(*value) || std::abs(*value) > 1.0)
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.rotation");
            }
            result.config.pre_rotation.coeffs()[index] = static_cast<float>(*value);
        }
        if (std::abs(result.config.pre_rotation.squaredNorm() - 1.0F) > 0.0001F)
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.recipe.rotation");
        }
        if (replacement.empty())
        {
            std::error_code ec;
            const auto parent = std::filesystem::weakly_canonical(file.parent_path(), ec);
            if (ec)
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.root");
            }
            const auto resolved = std::filesystem::weakly_canonical(result.root, ec);
            if (ec)
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.root");
            }
            const auto relative = resolved.lexically_relative(parent).generic_string();
            if (!validProjectPath(relative))
            {
                return failed(EEditorError::SOURCE_FAILURE, "model.recipe.root", relative);
            }
            auto captured = lux::toolchain::readModelSourceFiles(resolved, paths, source_limit);
            if (!captured)
            {
                return cookFailed(std::move(captured.error()));
            }
            for (std::size_t index{}; index < paths.size(); ++index)
            {
                const auto &captured_file = (*captured)[index];
                if (captured_file.state != lux::toolchain::EModelSourceState::PRESENT ||
                    projectContentDigest(captured_file.bytes.view()) != digests[index])
                {
                    return failed(EEditorError::SOURCE_FAILURE, "model.recipe.source-conflict", paths[index]);
                }
            }
            result.capture.files = std::move(*captured);
        }
        if (!replacement.empty())
        {
            result.root = replacement.parent_path();
            result.capture.entry = replacement.filename().generic_string();
        }
        return result;
    }
};
struct Read final
{
    const Source *source;
    std::vector<std::string> paths;
    std::size_t remaining;
    EditorResult<std::vector<lux::toolchain::ModelSourceFile>> operator()() const noexcept
    {
        auto read = lux::toolchain::readModelSourceFiles(source->root, paths, remaining);
        if (!read)
        {
            return cookFailed(std::move(read.error()));
        }
        return std::move(*read);
    }
};
struct Cook final
{
    const Source *source;
    asset::AssetInfo info;
    EditorResult<lux::toolchain::ModelCookAttempt> operator()() const noexcept
    {
        return lux::toolchain::cookModel(info, source->capture, source->config);
    }
};
struct Output final
{
    ProjectUpdate update;
    std::shared_ptr<const asset::ModelAsset> model;
};
struct Encode final
{
    const Source *source;
    const lux::toolchain::ModelCookProduct *product;
    ProjectAssetEntry entry;
    std::string before;
    EditorResult<Output> operator()() const noexcept
    {
        lux::cxx::algorithm::Sha256 hash;
        std::vector<const lux::toolchain::ModelSourceFile *> ordered;
        for (const auto &file : source->capture.files)
        {
            if (file.state == lux::toolchain::EModelSourceState::PRESENT)
            {
                ordered.push_back(&file);
            }
        }
        std::ranges::sort(ordered, {}, [](const auto *file) { return file->path; });
        // File names and lengths separate otherwise ambiguous concatenations.
        for (const auto *file : ordered)
        {
            hash.update(std::to_string(file->path.size()) + ":" + file->path + ":" +
                        std::to_string(file->bytes.size()) + ":");
            hash.update(file->bytes.view());
        }
        const auto digest = hash.digest();
        const auto source_generation = projectContentDigest(digest);
        const auto directory = "Sources/" + source_generation;
        const auto recipe_parent = std::filesystem::path(entry.source_path).parent_path().generic_string();
        Output result;
        toml::array source_files;
        for (const auto *file : ordered)
        {
            const auto content_digest = projectContentDigest(file->bytes.view());
            source_files.push_back(toml::table{{"path", file->path}, {"digest", content_digest}});
            result.update.files.push_back(
                {recipe_parent + "/" + directory + "/" + file->path, "missing", file->bytes, true});
        }
        toml::array rotation;
        for (const auto value : source->config.pre_rotation.coeffs())
        {
            rotation.push_back(static_cast<double>(value));
        }
        toml::table recipe{{"format", "lux.editor.model-source"},
                           {"version", 1},
                           {"root", directory},
                           {"entry", source->capture.entry},
                           {"rotation", std::move(rotation)},
                           {"scale", static_cast<double>(source->config.uniform_scale)},
                           {"left_handed", source->config.make_left_handed},
                           {"animations", source->config.import_animations},
                           {"files", std::move(source_files)}};
        std::ostringstream text;
        text << recipe;
        auto encoded_source = own(text.str());
        std::vector<asset::PakWriteEntry> entries;
        const auto append = [&]<class Asset>(const std::shared_ptr<const Asset> &value,
                                             std::string path) -> EditorResult<void> {
            auto encoded = asset::TAssetSerDeser<Asset>::encode(*value, asset::AssetEncodeLimits{source_limit});
            if (!encoded)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "model.artifact.encode",
                                                          static_cast<std::uint64_t>(encoded.error().code),
                                                          {},
                                                          encoded.error()});
            }
            entries.push_back({value->id(), Asset::primary_magic, std::move(path), {}, own(std::move(*encoded))});
            return {};
        };
        auto encoded = append(product->model, entry.mount_path);
        const auto group = [&](const auto &values, std::string_view directory_name) -> EditorResult<void> {
            for (std::size_t index{}; index < values.size(); ++index)
            {
                const auto &asset = values[index];
                const auto display = std::string(asset->info().display_name.data());
                const auto name =
                    display.empty() ? std::to_string(index) : display + " (" + std::to_string(index) + ")";
                auto result = append(asset, entry.mount_path + "/" + std::string(directory_name) + "/" + name);
                if (!result)
                {
                    return result;
                }
            }
            return {};
        };
        if (encoded)
        {
            encoded = group(product->meshes, "Meshes");
        }
        if (encoded)
        {
            encoded = group(product->materials, "Materials");
        }
        if (encoded)
        {
            encoded = group(product->textures, "Textures");
        }
        if (encoded)
        {
            encoded = group(product->animations, "Animations");
        }
        if (encoded && product->skeleton)
        {
            encoded = append(*product->skeleton, entry.mount_path + "/Skeleton");
        }
        if (!encoded)
        {
            return lux::cxx::unexpected(std::move(encoded.error()));
        }
        auto package = asset::encodePak(entries, source_limit, "/Project");
        if (!package)
        {
            return failed(EEditorError::SOURCE_FAILURE, "model.artifact.package", std::move(package.error()));
        }
        auto bytes = own(std::move(*package));
        auto published = entry;
        published.source_digest = projectContentDigest(encoded_source.view());
        published.compiled_source_digest = published.source_digest;
        published.cooked_path =
            ".lux/compiled/" + uuids::to_string(entry.id.uuid()) + "/" + projectContentDigest(bytes.view()) + ".luxpak";
        result.update.files.push_back({published.cooked_path, "missing", std::move(bytes), true});
        result.update.files.push_back({published.source_path, before, std::move(encoded_source)});
        result.update.assets.push_back(std::move(published));
        result.model = product->model;
        return result;
    }
};
} // namespace

struct AssetImporter::Data final
{
    struct Idle final
    {
    };
    using Loading = detail::ScheduledDocumentTask<process::BlockingScheduler, Load>;
    using Reading = detail::ScheduledDocumentTask<process::BlockingScheduler, Read>;
    using Cooking = detail::ScheduledDocumentTask<process::CpuScheduler, Cook>;
    using Encoding = detail::ScheduledDocumentTask<process::CpuScheduler, Encode>;
    struct Request final
    {
        Data &owner;
        AssetImportId id;
        ProjectAssetEntry entry;
        std::string before;
        Load load;
        Source source;
        std::vector<std::string> requested;
        lux::toolchain::ModelCookProduct product;
        Output output;
        AssetImportStatus status{AssetImportPending{EAssetImportStage::READING}};
        std::size_t bytes{}, rounds{};
        bool abandoning{};
        std::variant<Idle, Loading, Reading, Cooking, Encoding, detail::ProjectWrite> work;
        EAssetImportStage failed_stage{EAssetImportStage::READING};
        bool loaded{};

        Request(Data &data, AssetImportId request, ProjectAssetEntry asset, Load input)
            : owner(data), id(request), entry(std::move(asset)), before(data.project.sourceDigest(entry.source_path)),
              load(std::move(input))
        {
            load.expected_digest = before;
            startLoad();
        }
        void pending(EAssetImportStage stage)
        {
            status = AssetImportPending{stage, source.capture.files.size(), bytes};
            failed_stage = stage;
        }
        void startLoad()
        {
            pending(EAssetImportStage::READING);
            work.emplace<Loading>(owner.runtime, stdexec::then(stdexec::schedule(*owner.runtime.blocking()), load))
                .start();
        }
        void read()
        {
            pending(EAssetImportStage::READING);
            work.emplace<Reading>(owner.runtime, stdexec::then(stdexec::schedule(*owner.runtime.blocking()),
                                                               Read{&source, requested, source_limit - bytes}))
                .start();
        }
        void cook()
        {
            pending(EAssetImportStage::COOKING);
            asset::AssetInfo info;
            info.id = entry.id;
            info.type = asset::ModelAsset::asset_type;
            const auto name = std::filesystem::path(entry.mount_path).filename().string();
            std::copy_n(name.begin(), (std::min)(name.size(), info.display_name.size() - 1), info.display_name.begin());
            work.emplace<Cooking>(owner.runtime,
                                  stdexec::then(stdexec::schedule(owner.runtime.cpu()), Cook{&source, info}))
                .start();
        }
        void encode()
        {
            pending(EAssetImportStage::COOKING);
            work.emplace<Encoding>(owner.runtime, stdexec::then(stdexec::schedule(owner.runtime.cpu()),
                                                                Encode{&source, &product, entry, before}))
                .start();
        }
        bool terminal() const
        {
            return status.index() >= 2;
        }
        void abandon()
        {
            abandoning = true;
            if (auto *publication = std::get_if<detail::ProjectWrite>(&work))
            {
                publication->abandon();
            }
            else if (work.index() == 0)
            {
                status = AssetImportAbandoned{};
            }
        }
        EditorResult<void> retry()
        {
            if (!std::holds_alternative<EditorFailure>(status))
            {
                return failed(EEditorError::BUSY, "model.import.retry");
            }
            if (auto *publication = std::get_if<detail::ProjectWrite>(&work))
            {
                auto result = publication->retry();
                if (result)
                {
                    pending(abandoning ? EAssetImportStage::ABANDONING : EAssetImportStage::PUBLISHING);
                }
                return result;
            }
            if (abandoning)
            {
                status = AssetImportAbandoned{};
                return {};
            }
            if (!loaded)
            {
                startLoad();
            }
            else if (failed_stage == EAssetImportStage::READING)
            {
                read();
            }
            else if (failed_stage == EAssetImportStage::COOKING)
            {
                if (product.model)
                {
                    encode();
                }
                else
                {
                    requested.clear();
                    std::erase_if(source.capture.files, [&](const auto &file) {
                        if (file.state == lux::toolchain::EModelSourceState::MISSING)
                        {
                            requested.push_back(file.path);
                            return true;
                        }
                        return false;
                    });
                    if (requested.empty())
                    {
                        cook();
                    }
                    else
                    {
                        read();
                    }
                }
            }
            else
            {
                pending(EAssetImportStage::WAITING_FOR_PROJECT);
            }
            return {};
        }
        void poll()
        {
            if (auto *value = std::get_if<Loading>(&work); value && value->ready())
            {
                auto result = value->take();
                work.emplace<Idle>();
                if (abandoning)
                {
                    status = AssetImportAbandoned{};
                    return;
                }
                if (!result)
                {
                    status = std::move(result.error());
                }
                else
                {
                    source = std::move(*result);
                    loaded = true;
                    bytes = 0;
                    for (const auto &file : source.capture.files)
                    {
                        bytes += file.bytes.size();
                    }
                    if (source.capture.files.empty())
                    {
                        requested = {source.capture.entry};
                        read();
                    }
                    else
                    {
                        cook();
                    }
                }
            }
            if (auto *value = std::get_if<Reading>(&work); value && value->ready())
            {
                auto result = value->take();
                work.emplace<Idle>();
                if (abandoning)
                {
                    status = AssetImportAbandoned{};
                    return;
                }
                if (!result)
                {
                    status = std::move(result.error());
                }
                else
                {
                    for (auto &file : *result)
                    {
                        bytes += file.bytes.size();
                        source.capture.files.push_back(std::move(file));
                    }
                    if (source.capture.files.size() > 4096 || ++rounds > 32)
                    {
                        status = EditorFailure{EEditorError::CAPACITY, "model.source.closure"};
                    }
                    else
                    {
                        cook();
                    }
                }
            }
            if (auto *value = std::get_if<Cooking>(&work); value && value->ready())
            {
                auto result = value->take();
                work.emplace<Idle>();
                if (abandoning)
                {
                    status = AssetImportAbandoned{};
                    return;
                }
                if (!result)
                {
                    status = std::move(result.error());
                }
                else if (auto *next = std::get_if<lux::toolchain::ModelSourceRequests>(&*result))
                {
                    requested = std::move(next->paths);
                    read();
                }
                else if (auto *error = std::get_if<lux::toolchain::ModelCookFailure>(&*result))
                {
                    status = cookFailed(std::move(*error)).value();
                }
                else
                {
                    product = std::move(std::get<lux::toolchain::ModelCookProduct>(*result));
                    encode();
                }
            }
            if (auto *value = std::get_if<Encoding>(&work); value && value->ready())
            {
                auto result = value->take();
                work.emplace<Idle>();
                if (abandoning)
                {
                    status = AssetImportAbandoned{};
                    return;
                }
                if (!result)
                {
                    status = std::move(result.error());
                }
                else
                {
                    output = std::move(*result);
                    pending(EAssetImportStage::WAITING_FOR_PROJECT);
                }
            }
            if (auto *publication = std::get_if<detail::ProjectWrite>(&work))
            {
                publication->poll();
                const auto &state = publication->status();
                if (const auto *error = std::get_if<EditorFailure>(&state))
                {
                    status = *error;
                }
                if (const auto *done = std::get_if<detail::PublicationSucceeded>(&state))
                {
                    status = AssetImportSucceeded{entry.id, output.model, done->cleanup};
                    work.emplace<Idle>();
                }
                else if (std::holds_alternative<detail::PublicationAbandoned>(state))
                {
                    status = AssetImportAbandoned{};
                    work.emplace<Idle>();
                }
            }
            if (abandoning)
            {
                if (work.index() == 0 && !terminal())
                {
                    status = AssetImportAbandoned{};
                }
                return;
            }
            const auto *state = std::get_if<AssetImportPending>(&status);
            if (state && state->stage == EAssetImportStage::WAITING_FOR_PROJECT)
            {
                if (owner.project.sourceDigest(entry.source_path) != before)
                {
                    status = EditorFailure{EEditorError::STALE_REQUEST, "model.import.source"};
                    return;
                }
                const auto *current = owner.project.asset(entry.id);
                const auto &next = output.update.assets.front();
                if (current && current->cooked_path == next.cooked_path)
                {
                    std::erase_if(output.update.files, [&](const auto &file) { return file.path == next.cooked_path; });
                }
                auto prepared = owner.project.preparePublication(output.update);
                if (!prepared)
                {
                    if (prepared.error().code != EEditorError::BUSY)
                    {
                        status = std::move(prepared.error());
                    }
                    return;
                }
                pending(EAssetImportStage::PUBLISHING);
                work.emplace<detail::ProjectWrite>(owner.project, owner.runtime, std::move(*prepared));
            }
        }
    };
    Project &project;
    process::ExecutionRuntime &runtime;
    std::uint64_t owner{}, serial{};
    bool closing{};
    std::variant<Idle, Request> request;
    Data(Project &project, process::ExecutionRuntime &runtime) : project(project), runtime(runtime)
    {
        static std::atomic<std::uint64_t> counter{1};
        auto candidate = counter.load(std::memory_order_relaxed);
        while (candidate != UINT64_MAX &&
               !counter.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
        {
        }
        owner = candidate == UINT64_MAX ? 0 : candidate;
    }
    Request *find(AssetImportId id)
    {
        auto *active = std::get_if<Request>(&request);
        return active && active->id == id ? active : nullptr;
    }
};

AssetImporter::AssetImporter(Project &project, process::ExecutionRuntime &runtime)
    : data_(std::make_unique<Data>(project, runtime))
{
}
AssetImporter::~AssetImporter() = default;
EditorResult<AssetImportId> AssetImporter::requestModel(const ModelImportRequest &request)
{
    if (data_->closing)
    {
        return failed(EEditorError::CLOSING, "model.import");
    }
    if (!data_->project.writable())
    {
        return failed(EEditorError::READ_ONLY, "model.import");
    }
    if (data_->request.index() != 0)
    {
        return failed(EEditorError::BUSY, "model.import");
    }
    if (!data_->owner || data_->serial == UINT64_MAX)
    {
        return failed(EEditorError::CAPACITY, "model.import.identity");
    }
    if (request.asset.isNull() || !request.file.is_absolute() || !validProjectPath(request.browser_path) ||
        data_->project.asset(request.asset) || !data_->runtime.blocking())
    {
        return failed(EEditorError::INVALID_ARGUMENT, "model.import.request");
    }
    const auto id = AssetImportId{data_->owner, ++data_->serial};
    ProjectAssetEntry entry{request.asset,
                            EProjectAssetKind::MODEL,
                            "Assets/" + uuids::to_string(request.asset.uuid()) + "/Model.luxmodel",
                            {},
                            {},
                            {},
                            request.browser_path};
    data_->request.emplace<Data::Request>(*data_, id, std::move(entry),
                                          Load{request.file, request.configuration, false});
    return id;
}
EditorResult<AssetImportId> AssetImporter::reimportModel(asset::AssetId asset, const std::filesystem::path &replacement)
{
    if (data_->closing)
    {
        return failed(EEditorError::CLOSING, "model.reimport");
    }
    if (!data_->project.writable())
    {
        return failed(EEditorError::READ_ONLY, "model.reimport");
    }
    if (data_->request.index() != 0)
    {
        return failed(EEditorError::BUSY, "model.reimport");
    }
    if (!data_->owner || data_->serial == UINT64_MAX)
    {
        return failed(EEditorError::CAPACITY, "model.reimport.identity");
    }
    const auto *entry = data_->project.asset(asset);
    if (!entry || entry->kind != EProjectAssetKind::MODEL || !data_->runtime.blocking() ||
        (!replacement.empty() && !replacement.is_absolute()))
    {
        return failed(EEditorError::INVALID_ARGUMENT, "model.reimport.request");
    }
    const auto id = AssetImportId{data_->owner, ++data_->serial};
    data_->request.emplace<Data::Request>(*data_, id, *entry,
                                          Load{data_->project.root() / entry->source_path, {}, true, replacement});
    return id;
}
EditorResult<AssetImportStatus> AssetImporter::status(AssetImportId id) const
{
    const auto *request = data_->find(id);
    if (!request)
    {
        return failed(EEditorError::STALE_REQUEST, "model.import.status");
    }
    return request->status;
}
EditorResult<void> AssetImporter::retry(AssetImportId id)
{
    auto *request = data_->find(id);
    if (!request)
    {
        return failed(EEditorError::STALE_REQUEST, "model.import.retry");
    }
    return request->retry();
}
EditorResult<void> AssetImporter::abandon(AssetImportId id)
{
    auto *request = data_->find(id);
    if (!request)
    {
        return failed(EEditorError::STALE_REQUEST, "model.import.abandon");
    }
    request->abandon();
    return {};
}
EditorResult<void> AssetImporter::acknowledge(AssetImportId id)
{
    auto *request = data_->find(id);
    if (!request)
    {
        return failed(EEditorError::STALE_REQUEST, "model.import.acknowledge");
    }
    if (!request->terminal())
    {
        return failed(EEditorError::BUSY, "model.import.acknowledge");
    }
    data_->request.emplace<Data::Idle>();
    return {};
}
void AssetImporter::poll(PollBudget &budget)
{
    if (auto *request = std::get_if<Data::Request>(&data_->request); request && budget.document_steps)
    {
        --budget.document_steps;
        request->poll();
    }
}
void AssetImporter::requestClose() noexcept
{
    data_->closing = true;
    if (auto *request = std::get_if<Data::Request>(&data_->request); request && !request->terminal())
    {
        request->abandon();
    }
}
CloseStatus AssetImporter::closeStatus() const
{
    if (!data_->closing)
    {
        return {};
    }
    const auto *request = std::get_if<Data::Request>(&data_->request);
    if (!request || request->terminal())
    {
        return {ECloseState::CLOSED};
    }
    if (const auto *error = std::get_if<EditorFailure>(&request->status))
    {
        return {ECloseState::CLOSING, "Resolve retained model publication", lux::cxx::unexpected(*error)};
    }
    return {ECloseState::CLOSING, "Model import and project publication"};
}
} // namespace lux::editor::assets
