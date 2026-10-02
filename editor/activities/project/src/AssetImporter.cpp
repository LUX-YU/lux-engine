#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/assets/AssetImporter.hpp>
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
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "model.cook",
                static_cast<std::uint64_t>(error.code),
                error.detail,
                std::move(error)
            });
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
                const bool is_invalid_stream = !stream;
                const bool is_invalid_size = size < 0 || size > 16U * 1024U * 1024U;
                const bool is_invalid_recipe = is_invalid_stream || is_invalid_size;
                if (is_invalid_recipe)
                {
                    return failed(EEditorError::SOURCE_FAILURE, "model.recipe.read", file.string());
                }
                std::string bytes(static_cast<std::size_t>(size), '\0');
                stream.seekg(0);
                stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                const bool is_incomplete_read = !stream;
                const bool has_extra_data = !is_incomplete_read && stream.peek() != std::char_traits<char>::eof();
                const bool is_bad_stream = stream.bad();
                const bool is_invalid_read = is_incomplete_read || has_extra_data || is_bad_stream;
                if (is_invalid_read)
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
                    return failed(
                        EEditorError::SOURCE_FAILURE,
                        "model.recipe.parse",
                        std::string(parsed.error().description())
                    );
                }
                auto& table = parsed.table();
                constexpr std::array known_fields{
                    "format",
                    "version",
                    "root",
                    "entry",
                    "scale",
                    "left_handed",
                    "animations",
                    "rotation",
                    "files"
                };
                for (const auto& [key, value] : table)
                {
                    if (std::ranges::find(known_fields, key.str()) == known_fields.end())
                    {
                        return failed(
                            EEditorError::SOURCE_FAILURE,
                            "model.recipe.unknown-field",
                            std::string(key.str())
                        );
                    }
                }
                const auto format = table["format"].value<std::string>();
                const auto version = table["version"].value<std::int64_t>();
                const auto root = table["root"].value<std::string>();
                const auto entry = table["entry"].value<std::string>();
                const auto scale = table["scale"].value<double>();
                const auto handed = table["left_handed"].value<bool>();
                const auto animated = table["animations"].value<bool>();
                const auto* rotation = table["rotation"].as_array();
                const auto* files = table["files"].as_array();
                const bool is_invalid_format = !format || *format != "lux.editor.model-source";
                const bool is_invalid_version = !version || *version != 1;
                const bool is_missing_path = !root || !entry;
                const bool is_invalid_path =
                    !is_missing_path && (!validProjectPath(*root) || !validProjectPath(*entry));
                const bool is_missing_config = !scale || !handed || !animated;
                const bool is_invalid_rotation = !rotation || rotation->size() != 4;
                const bool is_invalid_file_list = !files || files->size() > 4096;
                const bool is_invalid_schema = is_invalid_format || is_invalid_version || is_missing_path ||
                                               is_invalid_path || is_missing_config || is_invalid_rotation ||
                                               is_invalid_file_list;
                if (is_invalid_schema)
                {
                    return failed(EEditorError::SOURCE_FAILURE, "model.recipe.schema");
                }
                const bool is_nonfinite_scale = !std::isfinite(*scale);
                const bool is_nonpositive_scale = *scale <= 0.0;
                const bool is_excessive_scale = *scale > std::numeric_limits<float>::max();
                const bool is_invalid_scale = is_nonfinite_scale || is_nonpositive_scale || is_excessive_scale;
                if (is_invalid_scale)
                {
                    return failed(EEditorError::SOURCE_FAILURE, "model.recipe.scale");
                }
                std::vector<std::string> paths, digests;
                std::set<std::string> unique_paths;
                for (const auto& item : *files)
                {
                    const auto* record = item.as_table();
                    const bool is_missing_record = record == nullptr;
                    const bool is_invalid_record_size = !is_missing_record && record->size() != 2;
                    const bool is_invalid_record = is_missing_record || is_invalid_record_size;
                    if (is_invalid_record)
                    {
                        return failed(EEditorError::SOURCE_FAILURE, "model.recipe.files");
                    }
                    const auto path = (*record)["path"].value<std::string>();
                    const auto digest = (*record)["digest"].value<std::string>();
                    const bool is_missing_field = !path || !digest;
                    const bool is_invalid_path = path && !validProjectPath(*path);
                    const bool is_invalid_digest_length = digest && digest->size() != 64;
                    const bool is_invalid_digest_chars = digest && !std::ranges::all_of(*digest, [](char c) {
                                                             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                                         });
                    const bool is_invalid_file =
                        is_missing_field || is_invalid_path || is_invalid_digest_length || is_invalid_digest_chars;
                    if (is_invalid_file)
                    {
                        return failed(EEditorError::SOURCE_FAILURE, "model.recipe.files");
                    }
                    auto folded = *path;
                    for (auto& character : folded)
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
                    const bool is_missing_value = !value;
                    const bool is_nonfinite_value = value && !std::isfinite(*value);
                    const bool is_excessive_value = value && std::abs(*value) > 1.0;
                    const bool is_invalid_value = is_missing_value || is_nonfinite_value || is_excessive_value;
                    if (is_invalid_value)
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
                        const auto& captured_file = (*captured)[index];
                        const bool is_missing_source =
                            captured_file.state != lux::toolchain::EModelSourceState::PRESENT;
                        const bool is_digest_mismatch =
                            !is_missing_source && projectContentDigest(captured_file.bytes.view()) != digests[index];
                        const bool has_source_conflict = is_missing_source || is_digest_mismatch;
                        if (has_source_conflict)
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
            const Source* source;
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
            const Source* source;
            asset::AssetInfo info;
            EditorResult<lux::toolchain::VModelCookAttempt> operator()() const noexcept
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
            const Source* source;
            const lux::toolchain::ModelCookProduct* product;
            ProjectAssetEntry entry;
            EditorResult<Output> operator()() const noexcept
            {
                lux::cxx::algorithm::Sha256 hash;
                std::vector<const lux::toolchain::ModelSourceFile*> ordered;
                for (const auto& file : source->capture.files)
                {
                    if (file.state == lux::toolchain::EModelSourceState::PRESENT)
                    {
                        ordered.push_back(&file);
                    }
                }
                std::ranges::sort(ordered, {}, [](const auto* file) { return file->path; });
                // File names and lengths separate otherwise ambiguous concatenations.
                for (const auto* file : ordered)
                {
                    hash.update(
                        std::to_string(file->path.size()) + ":" + file->path + ":" +
                        std::to_string(file->bytes.size()) + ":"
                    );
                    hash.update(file->bytes.view());
                }
                const auto digest = hash.digest();
                const auto source_generation = projectContentDigest(digest);
                const auto directory = "Sources/" + source_generation;
                const auto recipe_parent = std::filesystem::path(entry.source_path).parent_path().generic_string();
                Output result;
                toml::array source_files;
                for (const auto* file : ordered)
                {
                    const auto content_digest = projectContentDigest(file->bytes.view());
                    source_files.push_back(toml::table{{"path", file->path}, {"digest", content_digest}});
                    result.update.files.push_back(
                        {recipe_parent + "/" + directory + "/" + file->path, "missing", file->bytes, true}
                    );
                }
                toml::array rotation;
                for (const auto value : source->config.pre_rotation.coeffs())
                {
                    rotation.push_back(static_cast<double>(value));
                }
                toml::table recipe{
                    {"format", "lux.editor.model-source"},
                    {"version", 1},
                    {"root", directory},
                    {"entry", source->capture.entry},
                    {"rotation", std::move(rotation)},
                    {"scale", static_cast<double>(source->config.uniform_scale)},
                    {"left_handed", source->config.make_left_handed},
                    {"animations", source->config.import_animations},
                    {"files", std::move(source_files)}
                };
                std::ostringstream text;
                text << recipe;
                auto encoded_source = own(text.str());
                std::vector<asset::PakWriteEntry> entries;
                const auto append = [&]<class Asset>(const std::shared_ptr<const Asset>& value, std::string path)
                    -> EditorResult<void> {
                    auto encoded = asset::TAssetSerDeser<Asset>::encode(*value, asset::AssetEncodeLimits{source_limit});
                    if (!encoded)
                    {
                        return lux::cxx::unexpected(EditorFailure{
                            EEditorError::SOURCE_FAILURE,
                            "model.artifact.encode",
                            static_cast<std::uint64_t>(encoded.error().code),
                            {},
                            encoded.error()
                        });
                    }
                    entries.push_back({value->id(), Asset::primary_magic, std::move(path), {}, own(std::move(*encoded))}
                    );
                    return {};
                };
                auto encoded = append(product->model, entry.mount_path);
                const auto group = [&](const auto& values, std::string_view directory_name) -> EditorResult<void> {
                    for (std::size_t index{}; index < values.size(); ++index)
                    {
                        const auto& asset = values[index];
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
                published.cooked_path = ".lux/compiled/" + uuids::to_string(entry.id.uuid()) + "/" +
                                        projectContentDigest(bytes.view()) + ".luxpak";
                result.update.files.push_back({published.cooked_path, "missing", std::move(bytes), true});
                published.source_path = recipe_parent + "/Model-" + published.source_digest + ".luxmodel";
                result.update.files.push_back({published.source_path, "missing", std::move(encoded_source), true});
                result.update.assets.push_back(std::move(published));
                result.model = product->model;
                return result;
            }
        };
    } // namespace

    struct AssetImporter::Impl final
    {
        EditorResult<AssetImportId> requestModel(const ModelImportRequest&);
        EditorResult<AssetImportId> reimportModel(asset::AssetId, const std::filesystem::path&);
        std::optional<AssetImportId> currentRequest() const noexcept
        {
            const auto* active = std::get_if<Request>(&request);
            return active ? std::optional{active->id} : std::nullopt;
        }
        EditorResult<VAssetImportStatus> status(AssetImportId) const;
        EditorResult<void> retry(AssetImportId);
        EditorResult<void> abandon(AssetImportId);
        EditorResult<void> acknowledge(AssetImportId);
        void adoptCompleted() noexcept;
        void requestClose() noexcept;
        CloseStatus closeStatus() const;
        struct Idle final
        {};
        struct Working final
        {};
        struct Request final
        {
            Impl& owner;
            AssetImportId id;
            ProjectAssetEntry entry;
            std::string before;
            Load load;
            Source source;
            std::vector<std::string> requested;
            lux::toolchain::ModelCookProduct product;
            Output output;
            VAssetImportStatus status{AssetImportPending{EAssetImportStage::READING}};
            std::size_t bytes{}, rounds{};
            bool abandoning{};
            std::variant<Idle, Working, ProjectPublicationOperation> work;
            std::variant<
                std::monostate,
                EditorResult<Source>,
                EditorResult<std::vector<lux::toolchain::ModelSourceFile>>,
                EditorResult<lux::toolchain::VModelCookAttempt>,
                EditorResult<Output>>
                result;
            process::TaskScope tasks;
            EAssetImportStage failed_stage{EAssetImportStage::READING};
            bool loaded{};

            Request(Impl& data, AssetImportId request, ProjectAssetEntry asset, Load input)
                : owner(data), id(request), entry(std::move(asset)),
                  before(data.project.sourceDigest(entry.source_path)), load(std::move(input)), tasks(data.runtime)
            {
                load.expected_digest = before;
                startLoad();
            }
            void pending(EAssetImportStage stage)
            {
                status = AssetImportPending{stage, source.capture.files.size(), bytes};
                failed_stage = stage;
            }
            template <class Scheduler, class Work>
            void submit(std::string name, Scheduler scheduler, Work operation) noexcept
            {
                work.emplace<Working>();
                process::TaskOptions options{name, "Import"};
                auto admitted = tasks.submit(
                    std::move(options),
                    [scheduler, name = std::move(name), operation = std::move(operation)](process::TaskReporter reporter
                    ) mutable noexcept {
                        return stdexec::then(
                            stdexec::schedule(scheduler),
                            [reporter, name = std::move(name), operation = std::move(operation)](
                            ) mutable noexcept -> decltype(operation()) {
                                reporter.setPhase(name);
                                if (reporter.stopToken().stop_requested())
                                    return failed(EEditorError::CANCELLED, "model.import.cancelled");
                                auto result = operation();
                                if (reporter.stopToken().stop_requested())
                                    return failed(EEditorError::CANCELLED, "model.import.cancelled");
                                reporter.setProgress(1, 1);
                                return result;
                            }
                        );
                    },
                    [this](auto&& completed) noexcept {
                        result = detail::taskResult(std::move(completed));
                        owner.adoption.request();
                    }
                );
                if (!admitted)
                {
                    work.emplace<Idle>();
                    status =
                        EditorFailure{EEditorError::EXECUTION_FAILURE, "model.import.submit", 0, {}, admitted.error()};
                    owner.adoption.request();
                }
            }
            void startLoad()
            {
                pending(EAssetImportStage::READING);
                submit("Read model recipe", *owner.runtime.blocking(), load);
            }
            void read()
            {
                pending(EAssetImportStage::READING);
                submit("Read model sources", *owner.runtime.blocking(), Read{&source, requested, source_limit - bytes});
            }
            void cook()
            {
                pending(EAssetImportStage::COOKING);
                asset::AssetInfo info;
                info.id = entry.id;
                info.type = asset::ModelAsset::asset_type;
                const auto name = std::filesystem::path(entry.mount_path).filename().string();
                std::copy_n(
                    name.begin(),
                    (std::min)(name.size(), info.display_name.size() - 1),
                    info.display_name.begin()
                );
                submit("Cook model", owner.runtime.cpu(), Cook{&source, info});
            }
            void encode()
            {
                pending(EAssetImportStage::COOKING);
                submit("Encode model", owner.runtime.cpu(), Encode{&source, &product, entry});
            }
            bool terminal() const
            {
                return status.index() >= 2;
            }
            void abandon()
            {
                abandoning = true;
                if (auto* publication = std::get_if<ProjectPublicationOperation>(&work))
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
                if (auto* publication = std::get_if<ProjectPublicationOperation>(&work))
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
                        std::erase_if(source.capture.files, [&](const auto& file) {
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
            void adoptCompleted()
            {
                if (auto* value = std::get_if<EditorResult<Source>>(&this->result))
                {
                    auto result = std::move(*value);
                    this->result.emplace<std::monostate>();
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
                        for (const auto& file : source.capture.files)
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
                if (auto* value =
                        std::get_if<EditorResult<std::vector<lux::toolchain::ModelSourceFile>>>(&this->result))
                {
                    auto result = std::move(*value);
                    this->result.emplace<std::monostate>();
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
                        for (auto& file : *result)
                        {
                            bytes += file.bytes.size();
                            source.capture.files.push_back(std::move(file));
                        }
                        const bool has_too_many_files = source.capture.files.size() > 4096;
                        const bool has_too_many_rounds = !has_too_many_files && ++rounds > 32;
                        const bool is_closure_limit_exceeded = has_too_many_files || has_too_many_rounds;
                        if (is_closure_limit_exceeded)
                        {
                            status = EditorFailure{EEditorError::CAPACITY, "model.source.closure"};
                        }
                        else
                        {
                            cook();
                        }
                    }
                }
                if (auto* value = std::get_if<EditorResult<lux::toolchain::VModelCookAttempt>>(&this->result))
                {
                    auto result = std::move(*value);
                    this->result.emplace<std::monostate>();
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
                    else if (auto* next = std::get_if<lux::toolchain::ModelSourceRequests>(&*result))
                    {
                        requested = std::move(next->paths);
                        read();
                    }
                    else if (auto* error = std::get_if<lux::toolchain::ModelCookFailure>(&*result))
                    {
                        status = cookFailed(std::move(*error)).value();
                    }
                    else
                    {
                        product = std::move(std::get<lux::toolchain::ModelCookProduct>(*result));
                        encode();
                    }
                }
                if (auto* value = std::get_if<EditorResult<Output>>(&this->result))
                {
                    auto result = std::move(*value);
                    this->result.emplace<std::monostate>();
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
                if (auto* publication = std::get_if<ProjectPublicationOperation>(&work))
                {
                    publication->update();
                    const auto& state = publication->status();
                    if (const auto* error = std::get_if<EditorFailure>(&state))
                    {
                        status = *error;
                    }
                    if (const auto* done = std::get_if<PublicationSucceeded>(&state))
                    {
                        status = AssetImportSucceeded{entry.id, output.model, done->cleanup};
                        work.emplace<Idle>();
                    }
                    else if (const auto* abandoned = std::get_if<PublicationAbandoned>(&state))
                    {
                        status = AssetImportAbandoned{abandoned->published_files};
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
                const auto* state = std::get_if<AssetImportPending>(&status);
                if (state && state->stage == EAssetImportStage::WAITING_FOR_PROJECT)
                {
                    if (owner.project.sourceDigest(entry.source_path) != before)
                    {
                        status = EditorFailure{EEditorError::STALE_REQUEST, "model.import.source"};
                        return;
                    }
                    const auto* current = owner.project.asset(entry.id);
                    const auto& next = output.update.assets.front();
                    if (current && current->cooked_path == next.cooked_path)
                    {
                        std::erase_if(output.update.files, [&](const auto& file) {
                            return file.path == next.cooked_path;
                        });
                    }
                    auto prepared = owner.project.preparePublication(output.update);
                    if (!prepared)
                    {
                        if (prepared.error().code != EEditorError::BUSY)
                        {
                            status = std::move(prepared.error());
                        }
                        else
                            owner.project.whenPublicationAvailable(owner.adoption.requester());
                        return;
                    }
                    pending(EAssetImportStage::PUBLISHING);
                    work.emplace<ProjectPublicationOperation>(
                        owner.project,
                        owner.runtime,
                        owner.writes,
                        owner.files,
                        owner.execution,
                        std::move(*prepared)
                    );
                }
            }
        };
        ProjectStorage& project;
        process::ExecutionRuntime& runtime;
        persistence::WriteCoordinator& writes;
        persistence::IArtifactStore& files;
        persistence::SaveExecution& execution;
        std::uint64_t owner{}, serial{};
        bool closing{};
        process::CompletionWork adoption;
        EditorResult<void> setup;
        std::variant<Idle, Request> request;
        Impl(
            ProjectStorage& project,
            process::ExecutionRuntime& runtime,
            persistence::WriteCoordinator& writes,
            persistence::IArtifactStore& files,
            persistence::SaveExecution& execution
        )
            : project(project), runtime(runtime), writes(writes), files(files), execution(execution),
              adoption(runtime, this, [](void* owner) noexcept { static_cast<Impl*>(owner)->adoptCompleted(); })
        {
            static std::atomic<std::uint64_t> counter{1};
            auto candidate = counter.load(std::memory_order_relaxed);
            while (candidate != UINT64_MAX &&
                   !counter.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
            {
            }
            owner = candidate == UINT64_MAX ? 0 : candidate;
        }
        ~Impl()
        {
            closing = true;
            if (auto* active = std::get_if<Request>(&request))
            {
                active->abandon();
                active->tasks.requestStop();
                if (!active->tasks.join())
                    std::terminate();
                active->adoptCompleted();
                // The publication member drains accepted IO through the shared owner before it dies.
                if (const auto* failure = std::get_if<EditorFailure>(&active->status))
                    log::error("asset.import", "{}: {}", failure->domain, failure->message);
            }
            adoption.cancel();
        }
        const Request* find(AssetImportId id) const
        {
            const auto* active = std::get_if<Request>(&request);
            return active && active->id == id ? active : nullptr;
        }
        Request* find(AssetImportId id)
        {
            auto* active = std::get_if<Request>(&request);
            return active && active->id == id ? active : nullptr;
        }
    };

    AssetImporter::AssetImporter(
        ProjectStorage& project,
        process::ExecutionRuntime& runtime,
        persistence::WriteCoordinator& writes,
        persistence::IArtifactStore& files,
        persistence::SaveExecution& execution
    )
        : impl_(std::make_unique<Impl>(project, runtime, writes, files, execution))
    {}
    AssetImporter::~AssetImporter() = default;
    EditorResult<AssetImportId> AssetImporter::Impl::requestModel(const ModelImportRequest& input)
    {
        if (!setup)
            return lux::cxx::unexpected(setup.error());
        if (closing)
        {
            return failed(EEditorError::CLOSING, "model.import");
        }
        if (!project.writable())
        {
            return failed(EEditorError::READ_ONLY, "model.import");
        }
        if (request.index() != 0)
        {
            return failed(EEditorError::BUSY, "model.import");
        }
        const bool is_invalid_owner = !owner;
        const bool is_identity_exhausted = serial == UINT64_MAX;
        if (is_invalid_owner || is_identity_exhausted)
        {
            return failed(EEditorError::CAPACITY, "model.import.identity");
        }
        const bool is_invalid_asset = input.asset.isNull();
        const bool is_invalid_file = !input.file.is_absolute();
        const bool is_invalid_browser_path = !validProjectPath(input.browser_path);
        const bool has_existing_asset = !is_invalid_asset && project.asset(input.asset) != nullptr;
        const bool is_missing_executor = !runtime.blocking();
        const bool is_invalid_request =
            is_invalid_asset || is_invalid_file || is_invalid_browser_path || has_existing_asset || is_missing_executor;
        if (is_invalid_request)
        {
            return failed(EEditorError::INVALID_ARGUMENT, "model.import.request");
        }
        const auto id = AssetImportId{owner, ++serial};
        ProjectAssetEntry entry{
            input.asset,
            EProjectAssetKind::MODEL,
            "Assets/" + uuids::to_string(input.asset.uuid()) + "/Model.luxmodel",
            {},
            {},
            {},
            input.browser_path
        };
        request.emplace<Impl::Request>(*this, id, std::move(entry), Load{input.file, input.configuration, false});
        return id;
    }
    EditorResult<AssetImportId> AssetImporter::Impl::reimportModel(
        asset::AssetId asset,
        const std::filesystem::path& replacement
    )
    {
        if (!setup)
            return lux::cxx::unexpected(setup.error());
        if (closing)
        {
            return failed(EEditorError::CLOSING, "model.reimport");
        }
        if (!project.writable())
        {
            return failed(EEditorError::READ_ONLY, "model.reimport");
        }
        if (request.index() != 0)
        {
            return failed(EEditorError::BUSY, "model.reimport");
        }
        const bool is_invalid_owner = !owner;
        const bool is_identity_exhausted = serial == UINT64_MAX;
        if (is_invalid_owner || is_identity_exhausted)
        {
            return failed(EEditorError::CAPACITY, "model.reimport.identity");
        }
        const auto* entry = project.asset(asset);
        const bool is_missing_asset = entry == nullptr;
        const bool is_invalid_kind = !is_missing_asset && entry->kind != EProjectAssetKind::MODEL;
        const bool is_missing_executor = !runtime.blocking();
        const bool is_invalid_replacement = !replacement.empty() && !replacement.is_absolute();
        const bool is_invalid_request =
            is_missing_asset || is_invalid_kind || is_missing_executor || is_invalid_replacement;
        if (is_invalid_request)
        {
            return failed(EEditorError::INVALID_ARGUMENT, "model.reimport.request");
        }
        const auto id = AssetImportId{owner, ++serial};
        request.emplace<Impl::Request>(
            *this,
            id,
            *entry,
            Load{project.root() / entry->source_path, {}, true, replacement}
        );
        return id;
    }
    EditorResult<VAssetImportStatus> AssetImporter::Impl::status(AssetImportId id) const
    {
        const auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.status");
        }
        return active->status;
    }
    EditorResult<void> AssetImporter::Impl::retry(AssetImportId id)
    {
        auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.retry");
        }
        adoption.request();
        return active->retry();
    }
    EditorResult<void> AssetImporter::Impl::abandon(AssetImportId id)
    {
        auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.abandon");
        }
        active->abandon();
        return {};
    }
    EditorResult<void> AssetImporter::Impl::acknowledge(AssetImportId id)
    {
        auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.acknowledge");
        }
        if (!active->terminal())
        {
            return failed(EEditorError::BUSY, "model.import.acknowledge");
        }
        this->request.emplace<Impl::Idle>();
        return {};
    }
    void AssetImporter::Impl::adoptCompleted() noexcept
    {
        if (auto* active = std::get_if<Request>(&request))
            active->adoptCompleted();
    }
    void AssetImporter::Impl::requestClose() noexcept
    {
        closing = true;
        if (auto* active = std::get_if<Impl::Request>(&this->request); active && !active->terminal())
        {
            if (!std::holds_alternative<ProjectPublicationOperation>(active->work))
                active->abandon();
            adoption.request();
        }
    }
    CloseStatus AssetImporter::Impl::closeStatus() const
    {
        if (!closing)
        {
            return {};
        }
        const auto* active = std::get_if<Impl::Request>(&this->request);
        if (!active || active->terminal())
        {
            return {ECloseState::CLOSED};
        }
        if (const auto* error = std::get_if<EditorFailure>(&active->status))
        {
            return {
                ECloseState::CLOSING,
                "Model publication requires reconciliation or abandonment",
                lux::cxx::unexpected(*error)
            };
        }
        return {ECloseState::CLOSING, "Model import and project publication"};
    }
    EditorResult<AssetImportId> AssetImporter::requestModel(const ModelImportRequest& input)
    {
        return impl_->requestModel(input);
    }
    EditorResult<AssetImportId> AssetImporter::reimportModel(asset::AssetId id, const std::filesystem::path& file)
    {
        return impl_->reimportModel(id, file);
    }
    EditorResult<VAssetImportStatus> AssetImporter::status(AssetImportId id) const
    {
        return impl_->status(id);
    }
    EditorResult<void> AssetImporter::retry(AssetImportId id)
    {
        return impl_->retry(id);
    }
    EditorResult<void> AssetImporter::abandon(AssetImportId id)
    {
        return impl_->abandon(id);
    }
    EditorResult<void> AssetImporter::acknowledge(AssetImportId id)
    {
        return impl_->acknowledge(id);
    }
    void AssetImporter::update() noexcept
    {
        impl_->adoptCompleted();
    }
    void AssetImporter::requestClose() noexcept
    {
        impl_->requestClose();
    }
    CloseStatus AssetImporter::closeStatus() const
    {
        return impl_->closeStatus();
    }
} // namespace lux::editor::assets

namespace lux::editor::assets
{
    std::optional<AssetImportId> AssetImporter::currentRequest() const noexcept
    {
        return impl_->currentRequest();
    }
}
