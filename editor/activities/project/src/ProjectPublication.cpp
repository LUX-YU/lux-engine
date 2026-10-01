#include <lux/engine/editor/PublicationProbe.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/editor/storage/ProjectPublication.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

#include <array>
#include <fstream>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace lux::editor
{
    namespace
    {
        constexpr std::size_t file_limit = 512U * 1024U * 1024U;
        constexpr auto journal_directory = ".lux-editor-publication";

        struct PathLess final
        {
            bool operator()(const std::filesystem::path& first, const std::filesystem::path& second) const noexcept
            {
#if defined(_WIN32)
                return CompareStringOrdinal(
                           first.c_str(),
                           static_cast<int>(first.native().size()),
                           second.c_str(),
                           static_cast<int>(second.native().size()),
                           TRUE
                       ) == CSTR_LESS_THAN;
#else
                return first < second;
#endif
            }
        };

        auto failed(
            EProjectPublicationError code,
            const std::filesystem::path& path,
            std::uint64_t platform = 0,
            std::size_t published = 0
        )
        {
            ProjectPublicationFailure cause{code, path, platform, published};
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.publication",
                static_cast<std::uint64_t>(code),
                path.string(),
                std::move(cause)
            });
        }

        void release(std::intptr_t handle) noexcept
        {
            if (handle == -1)
            {
                return;
            }
#if defined(_WIN32)
            CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
            ::close(static_cast<int>(handle));
#endif
        }

        std::string digest(std::span<const std::byte> bytes)
        {
            return storage::publicationDigest(bytes);
        }
        EditorResult<std::vector<std::byte>> read(const std::filesystem::path& path)
        {
            auto result = storage::readPublicationFile(path, file_limit);
            if (!result)
                return failed(EProjectPublicationError::READ, path, result.error().native_code);
            LUX_EDITOR_IO("publication-read", path, result->size());
            return std::move(*result);
        }
        EditorResult<void> write(const std::filesystem::path& path, std::span<const std::byte> bytes)
        {
            auto result = storage::writePublicationFile(path, bytes);
            if (!result)
            {
                const auto code = result.error().code == storage::EFilePublicationError::FLUSH
                                      ? EProjectPublicationError::FLUSH
                                      : EProjectPublicationError::WRITE;
                return failed(code, path, result.error().native_code);
            }
            LUX_EDITOR_IO("publication-write", path, bytes.size());
            return {};
        }
        EditorResult<void> replace(const std::filesystem::path& staged, const std::filesystem::path& target)
        {
            auto result = storage::replacePublicationFile(staged, target);
            if (!result)
                return failed(EProjectPublicationError::REPLACE, target, result.error().native_code);
            return {};
        }

        EditorResult<std::filesystem::path> targetPath(const std::filesystem::path& root, std::string_view path)
        {
            const std::filesystem::path relative_path{std::filesystem::u8path(path)};
            const auto first = relative_path.begin();
            const auto equivalent = [](const auto& left, const auto& right) {
                return !PathLess{}(left, right) && !PathLess{}(right, left);
            };
            const bool reserved = first != relative_path.end() &&
                                  (equivalent(*first, ".lux-editor.lock") || equivalent(*first, journal_directory));
            if (!validProjectPath(path) || reserved)
            {
                return failed(EProjectPublicationError::INVALID_PATH, root / std::filesystem::u8path(path));
            }
            std::error_code error;
            const auto parent = std::filesystem::weakly_canonical(root / std::filesystem::u8path(path), error);
            if (error)
            {
                return failed(
                    EProjectPublicationError::INVALID_PATH,
                    root / std::filesystem::u8path(path),
                    error.value()
                );
            }
            const auto relative = parent.lexically_relative(std::filesystem::weakly_canonical(root, error));
            if (error || relative.empty() || *relative.begin() == "..")
            {
                return failed(
                    EProjectPublicationError::INVALID_PATH,
                    root / std::filesystem::u8path(path),
                    error.value()
                );
            }
            return parent;
        }

        struct Record final
        {
            std::string path;
            std::string before;
            std::string after;
        };

        struct Journal final
        {
            std::string phase;
            std::vector<Record> records;
        };

        EditorResult<void> writeJournal(const std::filesystem::path& directory, const Journal& journal)
        {
            toml::array records;
            for (const auto& record : journal.records)
            {
                records.push_back(toml::table{{"path", record.path}, {"before", record.before}, {"after", record.after}}
                );
            }
            const toml::table table{
                {"format", "lux.editor.publication.v1"},
                {"phase", journal.phase},
                {"files", std::move(records)}
            };
            std::ostringstream text;
            text << table;
            const auto bytes = text.str();
            const auto staged = directory / "journal.next";
            auto written = write(staged, std::as_bytes(std::span(bytes)));
            if (!written)
            {
                return written;
            }
            return replace(staged, directory / "journal.toml");
        }

        EditorResult<Journal> readJournal(const std::filesystem::path& directory)
        {
            auto bytes = read(directory / "journal.toml");
            if (!bytes || bytes->size() > 1024U * 1024U)
            {
                return failed(EProjectPublicationError::JOURNAL, directory);
            }
            auto parsed = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            if (!parsed || parsed["format"].value_or(std::string_view{}) != "lux.editor.publication.v1")
            {
                return failed(EProjectPublicationError::JOURNAL, directory);
            }
            Journal result{parsed["phase"].value_or(std::string{}), {}};
            const auto* files = parsed["files"].as_array();
            if (!files || files->empty() || files->size() > 4096 ||
                (result.phase != "PREPARING" && result.phase != "PREPARED" && result.phase != "COMMITTED"))
            {
                return failed(EProjectPublicationError::JOURNAL, directory);
            }
            std::set<std::filesystem::path, PathLess> unique;
            for (const auto& item : *files)
            {
                const auto* row = item.as_table();
                if (!row)
                {
                    return failed(EProjectPublicationError::JOURNAL, directory);
                }
                Record record{
                    (*row)["path"].value_or(std::string{}),
                    (*row)["before"].value_or(std::string{}),
                    (*row)["after"].value_or(std::string{})
                };
                if (!validProjectPath(record.path) || !unique.emplace(record.path).second ||
                    (record.before != "missing" && record.before.size() != 64) || record.after.size() != 64)
                {
                    return failed(EProjectPublicationError::JOURNAL, directory);
                }
                result.records.push_back(std::move(record));
            }
            return result;
        }

        EditorResult<void> cleanJournal(const std::filesystem::path& directory, const Journal& journal)
        {
            std::error_code error;
            for (std::size_t index{}; index < journal.records.size(); ++index)
            {
                for (const auto suffix : {".old", ".new", ".publish"})
                {
                    std::filesystem::remove(directory / (std::to_string(index) + suffix), error);
                    if (error)
                    {
                        return failed(EProjectPublicationError::JOURNAL, directory, error.value());
                    }
                }
                LUX_EDITOR_PUBLICATION_BOUNDARY("cleanup-record", index);
            }
            for (const auto* name : {"journal.next", "journal.toml"})
            {
                std::filesystem::remove(directory / name, error);
                if (error)
                {
                    return failed(EProjectPublicationError::JOURNAL, directory, error.value());
                }
            }
            LUX_EDITOR_PUBLICATION_BOUNDARY("cleanup-journal", 0);
            std::filesystem::remove(directory, error);
            if (error)
            {
                return failed(EProjectPublicationError::JOURNAL, directory, error.value());
            }
            return {};
        }
    } // namespace

    ProjectWriteLease::~ProjectWriteLease()
    {
        release(handle_);
    }
    ProjectWriteLease::ProjectWriteLease(ProjectWriteLease&& other) noexcept : handle_(std::exchange(other.handle_, -1))
    {}
    ProjectWriteLease& ProjectWriteLease::operator=(ProjectWriteLease&& other) noexcept
    {
        if (this != &other)
        {
            release(handle_);
            handle_ = std::exchange(other.handle_, -1);
        }
        return *this;
    }

    EditorResult<ProjectWriteLease> ProjectWriteLease::acquire(const std::filesystem::path& root)
    {
        const auto path = root / ".lux-editor.lock";
#if defined(_WIN32)
        const auto file = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN,
            nullptr
        );
        if (file == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION)
            {
                return ProjectWriteLease{};
            }
            return failed(EProjectPublicationError::WRITE, path, error);
        }
        return ProjectWriteLease{reinterpret_cast<std::intptr_t>(file)};
#else
        const auto file = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (file < 0)
        {
            return failed(EProjectPublicationError::WRITE, path, errno);
        }
        if (::flock(file, LOCK_EX | LOCK_NB) != 0)
        {
            const auto error = errno;
            ::close(file);
            if (error == EWOULDBLOCK)
            {
                return ProjectWriteLease{};
            }
            return failed(EProjectPublicationError::WRITE, path, error);
        }
        return ProjectWriteLease{file};
#endif
    }

    EditorResult<std::string> projectFileDigest(const std::filesystem::path& path)
    {
        auto result = storage::publicationFileDigest(path);
        if (!result)
            return failed(EProjectPublicationError::READ, path, result.error().native_code);
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (!error)
        {
            LUX_EDITOR_IO("hash-read", path, size);
        }
        return std::move(*result);
    }

    std::string projectContentDigest(std::span<const std::byte> bytes)
    {
        return digest(bytes);
    }

    EditorResult<void> recoverProjectFiles(const std::filesystem::path& root)
    {
        const auto directory = root / journal_directory;
        std::error_code error;
        if (!std::filesystem::exists(directory, error))
        {
            if (error)
            {
                return failed(EProjectPublicationError::JOURNAL, directory, error.value());
            }
            return {};
        }
        if (std::filesystem::is_empty(directory, error))
        {
            std::filesystem::remove(directory, error);
            if (error)
            {
                return failed(EProjectPublicationError::JOURNAL, directory, error.value());
            }
            return {};
        }
        if (error)
        {
            return failed(EProjectPublicationError::JOURNAL, directory, error.value());
        }
        // Before the first journal replacement only journal.next can exist; no source writes have begun.
        const bool has_journal = std::filesystem::exists(directory / "journal.toml", error);
        if (error)
        {
            return failed(EProjectPublicationError::JOURNAL, directory, error.value());
        }
        if (!has_journal)
        {
            const std::filesystem::directory_iterator entries(directory, error);
            if (error)
            {
                return failed(EProjectPublicationError::JOURNAL, directory, error.value());
            }
            for (auto entry = entries; entry != std::filesystem::directory_iterator{}; entry.increment(error))
            {
                if (error || entry->path().filename() != "journal.next" || !entry->is_regular_file(error))
                {
                    return failed(EProjectPublicationError::JOURNAL, directory, error.value());
                }
            }
            if (error)
            {
                return failed(EProjectPublicationError::JOURNAL, directory, error.value());
            }
            return cleanJournal(directory, {});
        }
        auto journal = readJournal(directory);
        if (!journal)
        {
            return lux::cxx::unexpected(journal.error());
        }
        // Validate every target before undoing any publication. External edits are never overwritten.
        for (const auto& record : journal->records)
        {
            auto path = targetPath(root, record.path);
            if (!path)
            {
                return lux::cxx::unexpected(path.error());
            }
            auto current = projectFileDigest(*path);
            if (!current)
            {
                return lux::cxx::unexpected(current.error());
            }
            const bool committed = journal->phase == "COMMITTED";
            const bool preparing = journal->phase == "PREPARING";
            if ((preparing && *current != record.before) ||
                (*current != record.after && (committed || *current != record.before)))
            {
                return failed(EProjectPublicationError::RECOVERY_CONFLICT, *path);
            }
        }
        if (journal->phase == "PREPARED")
        {
            for (std::size_t index{}; index < journal->records.size(); ++index)
            {
                const auto& record = journal->records[index];
                const auto path = root / std::filesystem::u8path(record.path);
                auto current = projectFileDigest(path);
                if (!current)
                {
                    return lux::cxx::unexpected(current.error());
                }
                if (*current == record.before)
                {
                    continue;
                }
                if (record.before == "missing")
                {
                    std::filesystem::remove(path, error);
                    if (error)
                    {
                        return failed(EProjectPublicationError::REPLACE, path, error.value());
                    }
                    continue;
                }
                auto original = read(directory / (std::to_string(index) + ".old"));
                if (!original || digest(*original) != record.before)
                {
                    return failed(EProjectPublicationError::RECOVERY_CONFLICT, path);
                }
                const auto staged = directory / (std::to_string(index) + ".publish");
                auto restored = write(staged, *original);
                if (restored)
                {
                    restored = replace(staged, path);
                }
                if (!restored)
                {
                    return restored;
                }
            }
        }
        return cleanJournal(directory, *journal);
    }

    EditorResult<ProjectPackage> readProjectPackage(const std::filesystem::path& root, std::string path)
    {
        auto provider = asset::PakAssetProvider::loadFromFile(root / std::filesystem::u8path(path));
        if (!provider)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "project.pak", 0, path + ": " + provider.error()}
            );
        }
        const auto mount_root = (*provider)->mountHint();
        ProjectPackage package{std::move(path), {mount_root, *provider, 0}, {}};
        package.entries.reserve((*provider)->assetCount());
        (*provider)->enumerate([&](const asset::ProviderEntry& entry) {
            package.entries.push_back(entry);
            package.entries.back().vpath = mount_root + "/" + entry.vpath;
        });
        if (package.entries.size() != (*provider)->assetCount())
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "project.pak.index",
                0,
                package.path + ": incomplete package enumeration"
            });
        }
        return package;
    }

    EditorResult<ProjectPublicationReceipt> publishProjectFiles(
        const ProjectPublication& publication,
        std::stop_token stop
    )
    {
        auto manifest_bytes = publication.manifest_bytes;
        if (manifest_bytes.empty())
        {
            auto manifest = encodeProjectManifest(publication.manifest);
            if (!manifest)
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "project.codec",
                    static_cast<std::uint64_t>(manifest.error().code),
                    manifest.error().field,
                    manifest.error()
                });
            auto owner = std::make_shared<const std::string>(std::move(*manifest));
            manifest_bytes = lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span(*owner)));
        }
        auto files = publication.files;
        files.push_back({publication.manifest_path, publication.before_manifest_digest, std::move(manifest_bytes)});
        if (files.size() > 4096)
        {
            return failed(EProjectPublicationError::JOURNAL, publication.root);
        }
        const auto directory = publication.root / journal_directory;
        auto recovered = recoverProjectFiles(publication.root);
        if (!recovered)
        {
            return lux::cxx::unexpected(recovered.error());
        }
        Journal journal{"PREPARING", {}};
        std::set<std::filesystem::path, PathLess> unique;
        for (const auto& file : files)
        {
            auto path = targetPath(publication.root, file.path);
            if (!path)
            {
                return lux::cxx::unexpected(path.error());
            }
            if (!unique.insert(*path).second || file.bytes.size() > file_limit)
            {
                return failed(EProjectPublicationError::INVALID_PATH, *path);
            }
            auto current = projectFileDigest(*path);
            if (!current)
            {
                return lux::cxx::unexpected(current.error());
            }
            const auto after = digest(file.bytes.view());
            const bool immutable_package =
                std::ranges::find(publication.package_paths, file.path) != publication.package_paths.end();
            const bool reuse_identical =
                (immutable_package || file.reuse_identical) && file.before_digest == "missing" && *current == after;
            if (*current != file.before_digest && !reuse_identical)
            {
                return failed(EProjectPublicationError::CONFLICT, *path);
            }
            journal.records.push_back({file.path, *current, after});
        }
        std::error_code error;
        std::filesystem::create_directory(directory, error);
        if (error)
        {
            return failed(EProjectPublicationError::JOURNAL, directory, error.value());
        }
        auto staged = writeJournal(directory, journal);
        if (!staged)
        {
            return lux::cxx::unexpected(staged.error());
        }
        LUX_EDITOR_PUBLICATION_BOUNDARY("preparing", 0);
        for (std::size_t index{}; index < files.size(); ++index)
        {
            if (stop.stop_requested())
            {
                return failed(EProjectPublicationError::CANCELLED, directory);
            }
            const auto& file = files[index];
            if (journal.records[index].before != "missing")
            {
                auto original = read(publication.root / std::filesystem::u8path(file.path));
                if (!original || digest(*original) != journal.records[index].before)
                {
                    return failed(
                        EProjectPublicationError::CONFLICT,
                        publication.root / std::filesystem::u8path(file.path)
                    );
                }
                staged = write(directory / (std::to_string(index) + ".old"), *original);
                if (!staged)
                {
                    return lux::cxx::unexpected(staged.error());
                }
            }
            staged = write(directory / (std::to_string(index) + ".new"), file.bytes.view());
            if (!staged)
            {
                return lux::cxx::unexpected(staged.error());
            }
            LUX_EDITOR_PUBLICATION_BOUNDARY("staged", index);
        }
        journal.phase = "PREPARED";
        staged = writeJournal(directory, journal);
        if (!staged)
        {
            return lux::cxx::unexpected(staged.error());
        }
        LUX_EDITOR_PUBLICATION_BOUNDARY("prepared", 0);
        for (std::size_t index{}; index < files.size(); ++index)
        {
            const auto target = publication.root / std::filesystem::u8path(files[index].path);
            auto current = projectFileDigest(target);
            if (!current || *current != journal.records[index].before)
            {
                return failed(EProjectPublicationError::CONFLICT, target, 0, index);
            }
            const auto& record = journal.records[index];
            if (record.before == record.after)
            {
                continue;
            }
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
            {
                return failed(EProjectPublicationError::WRITE, target, error.value(), index);
            }
            const auto next = directory / (std::to_string(index) + ".publish");
            auto published = write(next, files[index].bytes.view());
            if (published)
            {
                published = replace(next, target);
            }
            if (!published)
            {
                auto failure = std::move(published.error());
                std::any_cast<ProjectPublicationFailure&>(failure.cause).published_files = index;
                return lux::cxx::unexpected(std::move(failure));
            }
            LUX_EDITOR_PUBLICATION_BOUNDARY("published", index);
        }
        // Each immutable package must be usable before the durable commit decision.
        // If preparation fails, the journal still owns rollback; retry uses the same source capture.
        std::vector<ProjectPackage> packages;
        packages.reserve(publication.package_paths.size());
        for (const auto& path : publication.package_paths)
        {
            auto package = readProjectPackage(publication.root, path);
            if (!package)
            {
                return lux::cxx::unexpected(package.error());
            }
            packages.push_back(std::move(*package));
        }
        // Allocate the successful result before recording the durable commit decision.
        std::vector<std::pair<std::string, std::string>> digests;
        digests.reserve(publication.files.size());
        for (std::size_t index{}; index < publication.files.size(); ++index)
        {
            const auto& record = journal.records[index];
            digests.emplace_back(record.path, record.after);
        }
        ProjectPublicationReceipt receipt{
            publication.manifest,
            journal.records.back().after,
            files.size(),
            {},
            std::move(digests),
            std::move(packages)
        };
        journal.phase = "COMMITTED";
        auto committed = writeJournal(directory, journal);
        if (!committed)
        {
            auto failure = std::move(committed.error());
            std::any_cast<ProjectPublicationFailure&>(failure.cause).published_files = files.size();
            return lux::cxx::unexpected(std::move(failure));
        }
        // COMMITTED is the durable decision. Cleanup can be completed on the next open.
        LUX_EDITOR_PUBLICATION_BOUNDARY("committed", 0);
        receipt.cleanup = cleanJournal(directory, journal);
        return receipt;
    }
} // namespace lux::editor
