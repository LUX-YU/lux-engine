#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/asset/AssetImporter.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <thread>
using namespace lux::editor;
lux::asset::AssetId identity(std::uint8_t n)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = n;
    return lux::asset::AssetId(bytes);
}
void write(const std::filesystem::path &file, std::string_view text)
{
    std::ofstream out(file, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(out.good());
}
struct Evidence
{
    bool done{}, closed{};
};
class Probe final
{

  public:
    Probe(std::filesystem::path source, Evidence &evidence, Project &project, lux::process::ExecutionRuntime &runtime)
        : source_(std::move(source)), evidence_(evidence), project_(&project), importer_(project, runtime)
    {
        assets::ModelImportRequest request{identity(2), source_, "Models/Triangle", {}};
        auto &importer = importer_;
        auto accepted = importer.requestModel(request);
        assert(accepted);
        id_ = *accepted;
        assert(!importer.requestModel(request));
        auto foreign = id_;
        ++foreign.owner;
        assert(!importer.status(foreign));
    }
    void poll(PollBudget &budget)
    {
        auto &importer = importer_;
        importer.poll(budget);
        if (closing_)
        {
            evidence_.closed = importer.closeStatus().state == ECloseState::CLOSED;
            return;
        }
        assert(std::chrono::steady_clock::now() - began_ < std::chrono::seconds(80));
        auto state = importer.status(id_);
        assert(state);
        if (auto *failure = std::get_if<EditorFailure>(&*state))
        {
            std::printf("IMPORT FAILURE stage=%u %s:%llu %s\n", stage_, failure->domain.c_str(), failure->reason,
                        failure->message.c_str());
        }
        if (stage_ == 10)
        {
            if (const auto *error = std::get_if<EditorFailure>(&*state))
            {
                assert(error->domain == "model.recipe.source-conflict");
                assert(project_->manifest().assets.size() == 1 && project_->asset(identity(2))->cooked_path == cooked_);
                write(copied_source_, copied_bytes_);
                assert(importer.retry(id_));
                stage_ = 1;
                std::puts("exact copied-source digest conflict: cooked asset/catalog unchanged; original source "
                          "restored and same request retried");
                return;
            }
        }
        if (stage_ == 3)
        {
            if (const auto *error = std::get_if<EditorFailure>(&*state))
            {
                assert(error->domain == "model.cook" &&
                       error->reason == static_cast<std::uint64_t>(lux::toolchain::EModelCookError::IO_FAILURE));
                assert(project_->manifest().assets.size() == 1 && !project_->asset(identity(3)));
                std::string texture(18, '\0');
                texture[2] = 2;
                texture[12] = 1;
                texture[14] = 1;
                texture[16] = 32;
                texture[17] = 0x28;
                texture.append("\x00\x80\xff\xff", 4);
                write(source_.parent_path() / "required.tga", texture);
                assert(importer.retry(id_));
                stage_ = 4;
                std::puts("exact missing external texture: same request retained, project unchanged; supplying "
                          "dependency then retry");
                return;
            }
        }
        assert(!std::holds_alternative<EditorFailure>(*state));
        const auto *done = std::get_if<assets::AssetImportSucceeded>(&*state);
        if (!done)
        {
            return;
        }
        if (stage_ == 4)
        {
            assert(done->asset == identity(3) && done->model && project_->manifest().assets.size() == 2);
            assert(importer.acknowledge(id_));
            auto next = importer.reimportModel(identity(2));
            assert(next);
            id_ = *next;
            evidence_.done = true;
            closing_ = true;
            importer.requestClose();
            return;
        }
        assert(done->asset == identity(2) && done->model && done->cleanup);
        const auto *entry = project_->asset(identity(2));
        assert(entry && !entry->cooked_path.empty());
        assert(entry->source_digest == entry->compiled_source_digest);
        assert(project_->catalogAsset(done->model->data().primitives.front().mesh));
        assert(project_->catalogAsset(done->model->data().primitives.front().material));
        auto opened = project_->assets().open(identity(2));
        assert(opened);
        std::printf("import stage=%u catalog=%zu package=%s\n", stage_, project_->catalog().size(),
                    entry->cooked_path.c_str());
        if (stage_ == 0)
        {
            cooked_ = entry->cooked_path;
            mesh_ = done->model->data().primitives.front().mesh;
            assert(importer.acknowledge(id_));
            assert(!importer.status(id_));
            std::filesystem::rename(source_, source_.string() + ".offline");
            for (const auto &file : std::filesystem::recursive_directory_iterator(project_->root() / "Assets"))
            {
                if (file.path().filename() == "triangle.obj")
                {
                    copied_source_ = file.path();
                    break;
                }
            }
            assert(!copied_source_.empty());
            std::ifstream copied(copied_source_, std::ios::binary);
            copied_bytes_.assign(std::istreambuf_iterator<char>(copied), {});
            copied.close();
            write(copied_source_, copied_bytes_ + "# outside modification\n");
            auto next = importer.reimportModel(identity(2));
            assert(next);
            id_ = *next;
            stage_ = 10;
        }
        else if (stage_ == 1)
        {
            assert(entry->cooked_path == cooked_ && done->model->data().primitives.front().mesh == mesh_);
            assert(importer.acknowledge(id_));
            write(source_, "o Triangle\nv 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n");
            auto next = importer.reimportModel(identity(2), source_);
            assert(next);
            id_ = *next;
            ++stage_;
        }
        else
        {
            assert(entry->cooked_path != cooked_ && done->model->data().primitives.front().mesh == mesh_);
            assert(importer.acknowledge(id_));
            write(source_.parent_path() / "textured.obj", "mtllib texture.mtl\no Textured\nv 0 0 0\nv 1 0 0\nv 0 1 "
                                                          "0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl paint\nf 1/1 2/2 3/3\n");
            write(source_.parent_path() / "texture.mtl", "newmtl paint\nKd 1 1 1\nmap_Kd required.tga\n");
            auto next =
                importer.requestModel({identity(3), source_.parent_path() / "textured.obj", "Models/Textured", {}});
            assert(next);
            id_ = *next;
            stage_ = 3;
        }
    }

  private:
    std::filesystem::path source_;
    Evidence &evidence_;
    Project *project_{};
    assets::AssetImporter importer_;
    assets::AssetImportId id_;
    std::string cooked_;
    std::filesystem::path copied_source_;
    std::string copied_bytes_;
    lux::asset::AssetId mesh_;
    unsigned stage_{};
    bool closing_{};
    std::chrono::steady_clock::time_point began_{std::chrono::steady_clock::now()};
};
int main(int argc, char **argv)
{
    assert(argc == 2);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    lux::meta::ReflectionRegistry::initRegistry();
    const std::filesystem::path root(argv[1]);
    assert(!std::filesystem::exists(root));
    std::filesystem::create_directories(root / "project");
    std::filesystem::create_directories(root / "external");
    const auto source = root / "external/triangle.obj";
    write(source, "o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    auto manifest = encodeProjectManifest({identity(1), "Model workflow", {}, {}});
    assert(manifest);
    write(root / "project/Project.luxproject", *manifest);
    Evidence evidence;
    auto runtime =
        lux::process::ExecutionRuntime::create({2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}});
    assert(runtime && runtime->blocking());
    lux::process::TaskScope tasks;
    lux::object::ObjectMessageQueue messages;
    auto prepared = readProjectSource(root / "project/Project.luxproject");
    assert(prepared);
    auto project = Project::open(*prepared, *runtime->blocking(), tasks, messages.dispatcherRef());
    assert(project);
    {
        Probe probe(source, evidence, **project, *runtime);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(80);
        while (!evidence.closed)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            PollBudget budget;
            assert(runtime->drainMain(budget.main_completions));
            static_cast<void>(messages.dispatchPending(budget.object_messages));
            probe.poll(budget);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    assert(evidence.done);
    (*project)->requestClose();
    while (true)
    {
        auto closed = (*project)->advanceClose();
        assert(closed);
        if (*closed)
        {
            break;
        }
        assert(runtime->drainMain(64));
        std::this_thread::yield();
    }
    project->reset();
    assert(stdexec::sync_wait(tasks.close()));
    messages.close();
    runtime->requestStop();
    assert(runtime->join());
    auto reopened = readProjectSource(root / "project/Project.luxproject");
    assert(reopened && reopened->mounts.size() == 2);
    std::puts("PASS model import Process CPU/Blocking/Main, source closure, copied-source reimport, changed geometry "
              "stable IDs, exact missing texture and same request retry, publication/VFS, reopen and active close");
}
