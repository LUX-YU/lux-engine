#include <algorithm>
#include <atomic>
#include <fstream>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/editor/assets/ModelImportRecipe.hpp>
#include <lux/engine/editor/assets/ModelImporter.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/platform/FilePath.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::assets
{
    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<ModelImporter, ModelImporter>(
                services::ServiceNameView{"lux.editor.assets.importer"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.editor.project.storage"},
             1,
             cxx::typeToken<ProjectStorage>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.process.execution"},
             1,
             cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.writes"},
             1,
             cxx::typeToken<persistence::WriteCoordinator>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.files"},
             1,
             cxx::typeToken<persistence::IArtifactStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.execution"},
             1,
             cxx::typeToken<persistence::SaveExecution>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ModelImporter>>
        createImporter(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto project = resolver.require<ProjectStorage>(0);
            if (!project)
            {
                return cxx::unexpected(std::move(project.error()));
            }
            auto runtime = resolver.require<process::ExecutionRuntime>(1);
            if (!runtime)
            {
                return cxx::unexpected(std::move(runtime.error()));
            }
            auto writes = resolver.get<persistence::WriteCoordinator>(2);
            if (!writes)
            {
                return cxx::unexpected(std::move(writes.error()));
            }
            auto files = resolver.get<persistence::IArtifactStore>(3);
            if (!files)
            {
                return cxx::unexpected(std::move(files.error()));
            }
            auto execution = resolver.get<persistence::SaveExecution>(4);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            return std::make_unique<ModelImporter>(
                project->get(),
                runtime->get(),
                std::move(*writes),
                std::move(*files),
                std::move(*execution)
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kModelImporterService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ModelImporter, createImporter>(
            services::ServiceNameView{"lux.editor.assets.importer"},
            contracts,
            dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* allocation) noexcept -> services::ServiceResult<bool>
        {
            const auto& importer = *static_cast<const ModelImporter*>(allocation);
            const auto request = importer.currentRequest();
            if (!request)
            {
                return true;
            }
            auto status = importer.status(*request);
            if (!status)
            {
                return cxx::unexpected(services::ServiceFailure{
                    services::EServiceError::FACTORY_FAILURE,
                    status.error().message,
                    status.error().domain,
                    static_cast<std::uint64_t>(status.error().code)
                });
            }
            // Errors may retain an unresolved publication; only the original close protocol
            // determines whether those responsibilities have actually ended.
            const bool has_terminal_fact = std::holds_alternative<ModelImportSucceeded>(*status) ||
                                           std::holds_alternative<ModelImportAbandoned>(*status);
            return has_terminal_fact || importer.closeStatus().state == EModelImportCloseState::CLOSED;
        };
        descriptor.maintain = [](void* allocation) noexcept -> services::ServiceResult<void>
        {
            static_cast<ModelImporter*>(allocation)->update();
            return {};
        };
        return descriptor;
    }();

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
                std::ifstream stream(lux::engine::platform::nativeFilePath(file), std::ios::binary | std::ios::ate);
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
                auto recipe_value = decodeModelImportRecipe(std::as_bytes(std::span(bytes)));
                if (!recipe_value)
                    return lux::cxx::unexpected(std::move(recipe_value.error()));
                Source result{
                    file.parent_path() / recipe_value->root,
                    {recipe_value->entry, {}},
                    recipe_value->configuration
                };
                std::vector<std::string> paths;
                paths.reserve(recipe_value->files.size());
                for (const auto& file : recipe_value->files)
                    paths.push_back(file.path);
                if (replacement.empty())
                {
                    std::error_code ec;
                    const auto parent = std::filesystem::weakly_canonical(
                        lux::engine::platform::nativeFilePath(file.parent_path()),
                        ec
                    );
                    if (ec)
                    {
                        return failed(EEditorError::SOURCE_FAILURE, "model.recipe.root");
                    }
                    const auto resolved =
                        std::filesystem::weakly_canonical(lux::engine::platform::nativeFilePath(result.root), ec);
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
                            !is_missing_source && projectContentDigest(captured_file.bytes.view()) != recipe_value->files[index].digest;
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
                ModelImportRecipe recipe{directory, source->capture.entry, source->config};
                for (const auto* file : ordered)
                {
                    const auto content_digest = projectContentDigest(file->bytes.view());
                    recipe.files.push_back({file->path, content_digest});
                    result.update.files.push_back(
                        {recipe_parent + "/" + directory + "/" + file->path, "missing", file->bytes, true}
                    );
                }
                auto recipe_bytes = encodeModelImportRecipe(recipe);
                if (!recipe_bytes)
                    return lux::cxx::unexpected(std::move(recipe_bytes.error()));
                auto encoded_source = own(std::move(*recipe_bytes));
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

    struct ModelImporter::Impl final
    {
        EditorResult<ModelImportId> requestModel(const ModelImportRequest&);
        EditorResult<ModelImportId> reimportModel(asset::AssetId, const std::filesystem::path&);
        std::optional<ModelImportId> currentRequest() const noexcept
        {
            const auto* active = std::get_if<Request>(&request);
            return active ? std::optional{active->id} : std::nullopt;
        }
        EditorResult<VModelImportStatus> status(ModelImportId) const;
        EditorResult<void> retry(ModelImportId);
        EditorResult<void> abandon(ModelImportId);
        EditorResult<void> acknowledge(ModelImportId);
        void adoptCompleted() noexcept;
        void requestClose() noexcept;
        ModelImportCloseStatus closeStatus() const;
        struct Idle final
        {};
        struct Working final
        {};
        struct Request final
        {
            Impl& owner;
            ModelImportId id;
            ProjectAssetEntry entry;
            std::string before;
            Load load;
            Source source;
            std::vector<std::string> requested;
            lux::toolchain::ModelCookProduct product;
            Output output;
            VModelImportStatus status{ModelImportPending{EModelImportStage::READING}};
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
            EModelImportStage failed_stage{EModelImportStage::READING};
            bool loaded{};

            Request(Impl& data, ModelImportId request, ProjectAssetEntry asset, Load input)
                : owner(data), id(request), entry(std::move(asset)),
                  before(data.project.sourceDigest(entry.source_path)), load(std::move(input)), tasks(data.runtime)
            {
                load.expected_digest = before;
                startLoad();
            }
            void pending(EModelImportStage stage)
            {
                status = ModelImportPending{stage, source.capture.files.size(), bytes};
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
                pending(EModelImportStage::READING);
                submit("Read model recipe", *owner.runtime.blocking(), load);
            }
            void read()
            {
                pending(EModelImportStage::READING);
                submit("Read model sources", *owner.runtime.blocking(), Read{&source, requested, source_limit - bytes});
            }
            void cook()
            {
                pending(EModelImportStage::COOKING);
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
                pending(EModelImportStage::COOKING);
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
                    status = ModelImportAbandoned{};
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
                        pending(abandoning ? EModelImportStage::ABANDONING : EModelImportStage::PUBLISHING);
                    }
                    return result;
                }
                if (abandoning)
                {
                    status = ModelImportAbandoned{};
                    return {};
                }
                if (!loaded)
                {
                    startLoad();
                }
                else if (failed_stage == EModelImportStage::READING)
                {
                    read();
                }
                else if (failed_stage == EModelImportStage::COOKING)
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
                    pending(EModelImportStage::WAITING_FOR_PROJECT);
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
                        status = ModelImportAbandoned{};
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
                        status = ModelImportAbandoned{};
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
                        status = ModelImportAbandoned{};
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
                        status = ModelImportAbandoned{};
                        return;
                    }
                    if (!result)
                    {
                        status = std::move(result.error());
                    }
                    else
                    {
                        output = std::move(*result);
                        pending(EModelImportStage::WAITING_FOR_PROJECT);
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
                        status = ModelImportSucceeded{entry.id, output.model, done->cleanup};
                        work.emplace<Idle>();
                    }
                    else if (const auto* abandoned = std::get_if<PublicationAbandoned>(&state))
                    {
                        status = ModelImportAbandoned{abandoned->published_files};
                        work.emplace<Idle>();
                    }
                }
                if (abandoning)
                {
                    if (work.index() == 0 && !terminal())
                    {
                        status = ModelImportAbandoned{};
                    }
                    return;
                }
                const auto* state = std::get_if<ModelImportPending>(&status);
                if (state && state->stage == EModelImportStage::WAITING_FOR_PROJECT)
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
                    pending(EModelImportStage::PUBLISHING);
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
        std::shared_ptr<persistence::WriteCoordinator> writes_owner_;
        std::shared_ptr<persistence::IArtifactStore> files_owner_;
        std::shared_ptr<persistence::SaveExecution> execution_owner_;
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
        const Request* find(ModelImportId id) const
        {
            const auto* active = std::get_if<Request>(&request);
            return active && active->id == id ? active : nullptr;
        }
        Request* find(ModelImportId id)
        {
            auto* active = std::get_if<Request>(&request);
            return active && active->id == id ? active : nullptr;
        }
    };

    ModelImporter::ModelImporter(
        ProjectStorage& project,
        process::ExecutionRuntime& runtime,
        persistence::WriteCoordinator& writes,
        persistence::IArtifactStore& files,
        persistence::SaveExecution& execution
    )
        : impl_(std::make_unique<Impl>(project, runtime, writes, files, execution))
    {}
    ModelImporter::ModelImporter(
        ProjectStorage& project,
        process::ExecutionRuntime& runtime,
        std::shared_ptr<persistence::WriteCoordinator> writes,
        std::shared_ptr<persistence::IArtifactStore> files,
        std::shared_ptr<persistence::SaveExecution> execution
    )
        : impl_(std::make_unique<Impl>(project, runtime, *writes, *files, *execution))
    {
        impl_->writes_owner_ = std::move(writes);
        impl_->files_owner_ = std::move(files);
        impl_->execution_owner_ = std::move(execution);
    }
    ModelImporter::~ModelImporter() = default;
    EditorResult<ModelImportId> ModelImporter::Impl::requestModel(const ModelImportRequest& input)
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
        const auto id = ModelImportId{owner, ++serial};
        ProjectAssetEntry entry{
            input.asset,
            "lux.model.source",
            "Assets/" + uuids::to_string(input.asset.uuid()) + "/Model.luxmodel",
            {},
            {},
            {},
            input.browser_path
        };
        request.emplace<Impl::Request>(*this, id, std::move(entry), Load{input.file, input.configuration, false});
        return id;
    }
    EditorResult<ModelImportId> ModelImporter::Impl::reimportModel(
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
        const bool is_invalid_kind = !is_missing_asset && entry->source_type != "lux.model.source";
        const bool is_missing_executor = !runtime.blocking();
        const bool is_invalid_replacement = !replacement.empty() && !replacement.is_absolute();
        const bool is_invalid_request =
            is_missing_asset || is_invalid_kind || is_missing_executor || is_invalid_replacement;
        if (is_invalid_request)
        {
            return failed(EEditorError::INVALID_ARGUMENT, "model.reimport.request");
        }
        const auto id = ModelImportId{owner, ++serial};
        request.emplace<Impl::Request>(
            *this,
            id,
            *entry,
            Load{project.root() / entry->source_path, {}, true, replacement}
        );
        return id;
    }
    EditorResult<VModelImportStatus> ModelImporter::Impl::status(ModelImportId id) const
    {
        const auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.status");
        }
        return active->status;
    }
    EditorResult<void> ModelImporter::Impl::retry(ModelImportId id)
    {
        auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.retry");
        }
        adoption.request();
        return active->retry();
    }
    EditorResult<void> ModelImporter::Impl::abandon(ModelImportId id)
    {
        auto* active = find(id);
        if (!active)
        {
            return failed(EEditorError::STALE_REQUEST, "model.import.abandon");
        }
        active->abandon();
        return {};
    }
    EditorResult<void> ModelImporter::Impl::acknowledge(ModelImportId id)
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
    void ModelImporter::Impl::adoptCompleted() noexcept
    {
        if (auto* active = std::get_if<Request>(&request))
            active->adoptCompleted();
    }
    void ModelImporter::Impl::requestClose() noexcept
    {
        closing = true;
        if (auto* active = std::get_if<Impl::Request>(&this->request); active && !active->terminal())
        {
            if (!std::holds_alternative<ProjectPublicationOperation>(active->work))
                active->abandon();
            adoption.request();
        }
    }
    ModelImportCloseStatus ModelImporter::Impl::closeStatus() const
    {
        if (!closing)
        {
            return {};
        }
        const auto* active = std::get_if<Impl::Request>(&this->request);
        if (!active || active->terminal())
        {
            return {EModelImportCloseState::CLOSED};
        }
        if (const auto* error = std::get_if<EditorFailure>(&active->status))
        {
            return {
                EModelImportCloseState::CLOSING,
                "Model publication requires reconciliation or abandonment",
                lux::cxx::unexpected(*error)
            };
        }
        return {EModelImportCloseState::CLOSING, "Model import and project publication"};
    }
    EditorResult<ModelImportId> ModelImporter::requestModel(const ModelImportRequest& input)
    {
        return impl_->requestModel(input);
    }
    EditorResult<ModelImportId> ModelImporter::reimportModel(asset::AssetId id, const std::filesystem::path& file)
    {
        return impl_->reimportModel(id, file);
    }
    EditorResult<VModelImportStatus> ModelImporter::status(ModelImportId id) const
    {
        return impl_->status(id);
    }
    EditorResult<void> ModelImporter::retry(ModelImportId id)
    {
        return impl_->retry(id);
    }
    EditorResult<void> ModelImporter::abandon(ModelImportId id)
    {
        return impl_->abandon(id);
    }
    EditorResult<void> ModelImporter::acknowledge(ModelImportId id)
    {
        return impl_->acknowledge(id);
    }
    void ModelImporter::update() noexcept
    {
        impl_->adoptCompleted();
    }
    void ModelImporter::requestClose() noexcept
    {
        impl_->requestClose();
    }
    ModelImportCloseStatus ModelImporter::closeStatus() const
    {
        return impl_->closeStatus();
    }
} // namespace lux::editor::assets

namespace lux::editor::assets
{
    std::optional<ModelImportId> ModelImporter::currentRequest() const noexcept
    {
        return impl_->currentRequest();
    }
}
