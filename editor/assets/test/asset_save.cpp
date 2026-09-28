#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/detail/AssetSource.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <thread>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    struct Encoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(const std::string& value, std::stop_token stop) const
        {
            if (stop.stop_requested())
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "test.encode"});
            auto storage = std::make_shared<const std::string>(value);
            return lux::cxx::SharedBytes<>::fromOwner(storage, std::as_bytes(std::span(*storage)));
        }
    };
    struct Codec final
    {
        using Source = std::pair<asset::AssetId, std::string>;
        static constexpr std::size_t max_bytes = 4096;
        static asset::AssetId identity(const Source& source) noexcept
        {
            return source.first;
        }
        static EditorResult<Source> decode(const lux::cxx::SharedBytes<>& bytes, std::stop_token)
        {
            const auto text = std::string(reinterpret_cast<const char*>(bytes.view().data()), bytes.size());
            return Source{asset::AssetId{*uuids::uuid::from_string(text.substr(0, 36))}, text.substr(36)};
        }
    };
}

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto project_id = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d932")};
    const auto first = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d933")};
    const auto second = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d934")};
    const auto manifest_path = root / "Project.luxproject";
    std::filesystem::create_directories(root / "Content");
    const auto source_path = root / "Content/New.luxmaterial";
    std::filesystem::remove(source_path);
    std::filesystem::remove(root / "Content/Conflict.luxmaterial");
    {
        std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
        out << *encodeProjectManifest(ProjectManifest{project_id, "Asset save", {}, {}});
    }
    auto execution = process::ExecutionRuntime::create({1, 64, 64, {64}, process::BlockingSchedulerConfig{1, 64}});
    auto messages = object::ObjectMessageQueue::create(32);
    assert(execution && messages);
    process::TaskScope tasks{*execution};
    auto source = readProjectOpenData(manifest_path);
    assert(source);
    lux::asset::AssetVfs assets;
    auto project = ProjectStorage::open(*source, assets, *execution->blocking(), tasks, messages->dispatcherRef());
    assert(project);
    auto history = editing::EditHistory::create({{32, 1024 * 1024, 1024 * 1024, 16}, {}});
    assert(history);
    transition::LegacyPersistenceState persistence;
    persistence.reset(**history, false);
    using Save = detail::TAssetSave<std::string, Encoder>;
    const auto await = [&](auto ready, auto poll) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(execution->collectCompletions());
            poll();
            std::this_thread::yield();
        }
    };
    assert(!detail::captureAssetSaveTarget(**project, first));
    assert(!detail::newAssetSaveTarget(**project, first, EProjectAssetKind::MATERIAL_GRAPH, "C:/New.luxmaterial"));
    assert(!detail::newAssetSaveTarget(**project, first, EProjectAssetKind::MATERIAL_GRAPH, "/Project/../New"));
    auto target =
        detail::newAssetSaveTarget(**project, first, EProjectAssetKind::MATERIAL_GRAPH, "/Project/New.luxmaterial");
    assert(target && !(*project)->asset(first));
    const auto initial = uuids::to_string(first.uuid()) + "initial";
    {
        auto ticket = persistence.capture();
        assert(ticket);
        Save save(
            {(*history)->id(), 1},
            *ticket,
            (*history)->view()->snapshot.revision,
            std::move(*target),
            initial,
            **project,
            *execution,
            persistence
        );
        await([&] { return save.terminal(); }, [] {});
        assert(std::holds_alternative<SaveSucceeded>(save.status()));
        assert(persistence.clean());
        assert((*project)->asset(first) && (*project)->asset(first)->cooked_path.empty());
        assert((*project)->asset(first)->source_digest == projectContentDigest(std::as_bytes(std::span(initial))));
    }
    assert(!detail::newAssetSaveTarget(**project, second, EProjectAssetKind::MATERIAL_GRAPH, "/Project/New.luxmaterial")
    );
    // No presenter or explicit polling: destruction finishes encoding, disk publication and Main adoption.
    {
        auto target = detail::captureAssetSaveTarget(**project, first);
        auto ticket = persistence.capture();
        assert(target && ticket);
        {
            Save save(
                {(*history)->id(), 3},
                *ticket,
                (*history)->view()->snapshot.revision,
                std::move(*target),
                initial,
                **project,
                *execution,
                persistence
            );
        }
        assert(persistence.clean());
        // The completed operation released its history ticket.
        auto extra = persistence.capture();
        assert(extra && persistence.settle(*extra, transition::ELegacyPersistenceOutcome::CANCELLED));
    }
    {
        std::optional<EditorResult<Codec::Source>> result;
        auto read = execution->submit(
            {"Read source", "test"},
            [&](process::TaskReporter reporter) noexcept {
                return detail::readAssetSource(**project, *(*project)->asset(first), *execution, reporter, Codec{});
            },
            [&](process::TTaskResult<Codec::Source, EditorFailure>&& value) noexcept {
                result.emplace(detail::taskResult(std::move(value)));
            }
        );
        assert(read);
        await([&] { return result.has_value(); }, [&] { assert(execution->dispatchTaskEvents()); });
        assert(*result && (*result)->first == first && (*result)->second == "initial");
        read = process::Task{};
        result.reset();
        {
            std::ofstream changed(source_path, std::ios::binary | std::ios::trunc);
            changed << uuids::to_string(first.uuid()) << "external change";
        }
        read = execution->submit(
            {"Read changed source", "test"},
            [&](process::TaskReporter reporter) noexcept {
                return detail::readAssetSource(**project, *(*project)->asset(first), *execution, reporter, Codec{});
            },
            [&](process::TTaskResult<Codec::Source, EditorFailure>&& value) noexcept {
                result.emplace(detail::taskResult(std::move(value)));
            }
        );
        assert(read);
        await([&] { return result.has_value(); }, [&] { assert(execution->dispatchTaskEvents()); });
        assert(!*result && result->error().domain == "asset.source.conflict");
    }
    assert((*history)->close());
    history->reset();
    (*project)->requestClose();
    await(
        [&] {
            auto closed = (*project)->advanceClose();
            assert(closed);
            return *closed;
        },
        [] {}
    );
    project->reset();
    assert(tasks.join());
    execution->requestStop();
    assert(execution->join());
}
