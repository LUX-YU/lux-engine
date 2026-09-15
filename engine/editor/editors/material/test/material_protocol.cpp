#include "FiniteCost.hpp"
#include "TestExit.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <thread>
using namespace lux::editor;
namespace mat = lux::editor::material;
namespace source = lux::material;
using MaterialNodeBorrow = std::ranges::range_value_t<decltype(std::declval<const source::MaterialGraph &>().nodes())>;
static_assert(std::is_same_v<MaterialNodeBorrow::second_type, const source::Node *>);

lux::asset::AssetId identity(std::uint8_t n)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = n;
    return lux::asset::AssetId{bytes};
}
void write(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), text.size());
    assert(file.good());
}
void fixture(const std::filesystem::path &root)
{
    assert(!std::filesystem::exists(root));
    std::filesystem::create_directories(root);
    source::MaterialSourceDocument document{identity(2), "Material source", {}};
    auto constant = std::make_unique<source::ConstantNode>();
    constant->setType(source::EValueType::VEC3);
    constant->value[0] = 0.25F;
    const auto node = document.graph.addNode(std::move(constant));
    const auto output = document.graph.addNode(std::make_unique<source::OutputSurfaceNode>());
    assert(node.valid() && output.valid() && document.graph.connect(node, 0, output, 0));
    auto encoded = source::encodeMaterialSource(document);
    assert(encoded);
    write(root / "Material.luxmaterial", *encoded);
    document.graph.removeNode(output);
    document.id = identity(3);
    auto broken = source::encodeMaterialSource(document);
    assert(broken);
    write(root / "Unfinished.luxmaterial", *broken);
    ProjectManifest project{identity(1),
                            "Material protocol",
                            {},
                            {{identity(2),
                              EProjectAssetKind::MATERIAL_GRAPH,
                              "Material.luxmaterial",
                              {},
                              projectContentDigest(std::as_bytes(std::span{encoded->data(), encoded->size()})),
                              {},
                              "Materials/Main"},
                             {identity(3),
                              EProjectAssetKind::MATERIAL_GRAPH,
                              "Unfinished.luxmaterial",
                              {},
                              projectContentDigest(std::as_bytes(std::span{broken->data(), broken->size()})),
                              {},
                              "Materials/Unfinished"}}};
    auto manifest = encodeProjectManifest(project);
    if (!manifest)
    {
        std::printf("manifest: %u %s\n", unsigned(manifest.error().code), manifest.error().field.c_str());
    }
    assert(manifest);
    write(root / "Project.luxproject", *manifest);
}

struct Evidence final
{
    bool completed{}, closed{};
};
class Probe final : public EditorFrontend
{
    TestExit exit_;

  public:
    explicit Probe(Evidence &evidence) : evidence_(evidence) {}
    EditorResult<void> beginStartup(Editor &editor, lux::process::ExecutionRuntime &runtime,
                                    lux::object::ObjectDispatcherRef) override
    {
        runtime_ = &runtime;
        return editor.registerDocument({std::string(mat::kMaterialDocumentType),
                                        [&runtime](Project &project, const OpenDocumentRequest &request)
                                        { return mat::openMaterialDocument(project, request, runtime); }});
    }
    EditorResult<void> enterProject(Editor &editor, Project &project, lux::process::ExecutionRuntime &,
                                    lux::object::ObjectDispatcherRef) override
    {
        editor_ = &editor;
        project_ = &project;
        open(identity(2));
        return {};
    }
    void collectInput(Editor &) override {}
    void poll(PollBudget &) override
    {
        exit_.poll();
        assert(std::chrono::steady_clock::now() - began_ < std::chrono::seconds(60));
        if (!project_ || evidence_.completed)
        {
            return;
        }
        if (stage_ == 0 || stage_ == 3 || stage_ == 6)
        {
            auto status = editor_->openStatus(open_);
            assert(status);
            if (std::holds_alternative<OpenPending>(*status))
            {
                return;
            }
            if (const auto *error = std::get_if<EditorFailure>(&*status))
            {
                std::printf("OPEN ERROR %s:%llu %s\n", error->domain.c_str(), error->reason, error->message.c_str());
            }
            assert(std::holds_alternative<DocumentHandle>(*status));
            document_ = std::get<DocumentHandle>(*status);
            assert(editor_->acknowledgeOpen(open_));
            auto &doc = document();
            if (stage_ == 0)
            {
                auto duplicate = editor_->requestOpen({doc.summary().key, "duplicate"});
                assert(duplicate);
                auto shared = editor_->openStatus(*duplicate);
                assert(shared && std::get<DocumentHandle>(*shared) == document_);
                assert(editor_->acknowledgeOpen(*duplicate));
                assert(doc.views().empty() && doc.historyView()->history.clean);
                const auto initial_shading = doc.source().graph.shading_model;
                assert(doc.setShadingModel(lux::rdesc::ELightingTechnique::Unlit));
                assert(doc.source().graph.shading_model == lux::rdesc::ELightingTechnique::Unlit);
                assert(doc.undo() && doc.source().graph.shading_model == initial_shading);
                assert(!doc.setShadingModel(static_cast<lux::rdesc::ELightingTechnique>(255)));
                const auto node = doc.source().graph.topology().nodes().front().id;
                measureEdits("Material.constant", doc, [&](unsigned index)
                             { return doc.setConstant(node, {0.01F * (index + 1), 0.25F, 0.125F, 0}); });

                {
                    const auto output = doc.source().graph.topology().nodes().back().id;
                    const auto original = source::encodeMaterialSource(doc.source());
                    assert(original);
                    auto added = std::unique_ptr<source::Node>(new source::ConstantNode);
                    auto *input = added.get();
                    auto invalid_insert = doc.insertNode(added, {std::numeric_limits<float>::quiet_NaN(), 0, true});
                    assert(!invalid_insert && added.get() == input && !added->id().valid());
                    auto created = doc.insertNode(added, {100, 120, true});
                    assert(created && !added && doc.source().graph.node(*created));
                    const auto from = doc.source().graph.node(*created)->outputs()[0].id;
                    const auto to = doc.source().graph.node(output)->inputs()[0].id;
                    assert(doc.connect(from, to));
                    assert(doc.source().graph.source(output, 0).node == *created);
                    assert(doc.undo() && doc.source().graph.source(output, 0).node == node);
                    const auto with_redo = doc.historyView()->history;
                    auto invalid_link = doc.connect(lux::graph::PinId{999999}, to);
                    assert(!invalid_link && invalid_link.error().code == editing::EEditError::PRECONDITION_FAILED);
                    assert(doc.historyView()->history.revision == with_redo.revision);
                    assert(doc.redo() && doc.source().graph.source(output, 0).node == *created);
                    const auto connected = source::encodeMaterialSource(doc.source());
                    const auto before = doc.historyView()->history;
                    const std::array invalid_batch{node, lux::graph::NodeId{999999}};
                    assert(!doc.removeNodes(invalid_batch));
                    assert(doc.historyView()->history.current == before.current &&
                           doc.historyView()->history.revision == before.revision);
                    assert(*source::encodeMaterialSource(doc.source()) == *connected);
                    const std::array batch{node, *created};
                    assert(doc.removeNodes(batch) && doc.source().graph.nodes().size() == 1);
                    assert(doc.undo() && *source::encodeMaterialSource(doc.source()) == *connected);
                    assert(doc.redo() && doc.undo() && *source::encodeMaterialSource(doc.source()) == *connected);
                    while (doc.historyView()->undo == editing::EHistoryActionAvailability::READY)
                    {
                        assert(doc.undo());
                    }
                    assert(*source::encodeMaterialSource(doc.source()) == *original);
                    assert(doc.historyView()->history.clean);
                    auto replacement = std::unique_ptr<source::Node>(new source::ConstantNode);
                    auto replacement_id = doc.insertNode(replacement);
                    assert(replacement_id && replacement_id->value > created->value && doc.undo());
                    assert(doc.moveNode(node, {32, 64, true}) && doc.undo());
                    assert(!doc.source().graph.layout().find(node));
                    assert(*source::encodeMaterialSource(doc.source()) == *original);
                    std::puts("graph journal: compound remove/restore stable node+pin IDs, displaced link undo, exact "
                              "failure state, branch IDs monotonic, input retained");
                }
                {
                    const auto output = doc.source().graph.topology().nodes().back().id;
                    const auto original = *source::encodeMaterialSource(doc.source());
                    const auto before = doc.historyView()->history;
                    auto draft = doc.source().graph.node(node)->clone();
                    auto *retained = draft.get();
                    draft->as<source::ConstantNode>()->setType(source::EValueType::VEC2);
                    auto rejected = doc.replaceNode(before.current, draft);
                    assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED);
                    assert(rejected.error().domain_code ==
                           static_cast<std::uint64_t>(lux::graph::EGraphTopologyError::INVALID_TYPE));
                    assert(draft.get() == retained && *source::encodeMaterialSource(doc.source()) == original);
                    assert(doc.historyView()->history.current == before.current);
                    draft->as<source::ConstantNode>()->setType(source::EValueType::VEC4);
                    assert(doc.replaceNode(before.current, draft) && !draft);
                    assert(doc.source().graph.source(output, 0).node == node);
                    auto stale = doc.source().graph.node(node)->clone();
                    auto mismatch = doc.replaceNode(before.current, stale);
                    assert(!mismatch && stale && mismatch.error().code == editing::EEditError::STALE_TARGET);
                    assert(doc.undo() && *source::encodeMaterialSource(doc.source()) == original);
                    assert(doc.redo() && doc.undo());
                    auto unchanged = doc.source().graph.node(node)->clone();
                    auto noop = doc.replaceNode(doc.historyView()->history.current, unchanged);
                    assert(noop && !unchanged && doc.historyView()->history.current == before.current);
                    auto added = std::unique_ptr<source::Node>(new source::ConstructNode);
                    auto created = doc.insertNode(added);
                    assert(created);
                    auto construct = doc.source().graph.node(*created)->clone();
                    construct->as<source::ConstructNode>()->setType(source::EValueType::VEC4);
                    assert(doc.replaceNode(doc.historyView()->history.current, construct) && !construct);
                    const auto fourth = doc.source().graph.node(*created)->inputs().back().id;
                    assert(fourth.valid() && doc.undo() && doc.source().graph.node(*created)->inputs().size() == 3);
                    assert(doc.redo() && doc.source().graph.node(*created)->inputs().size() == 4);
                    assert(doc.source().graph.node(*created)->inputs().back().id == fourth);
                    assert(doc.undo() && doc.undo() && *source::encodeMaterialSource(doc.source()) == original);
                    std::puts("node properties: exact incompatible-link rejection, retained draft retry, stale state, "
                              "no-op, pin-growth stable redo");
                }
                {
                    const auto before = doc.historyView()->history;
                    const auto original = *source::encodeMaterialSource(doc.source());
                    std::vector<source::TextureSlotDecl> textures{{"Albedo", {}}};
                    assert(doc.setTextureSlots(before.current, textures));
                    auto sample = std::unique_ptr<source::Node>(new source::SampleTextureNode);
                    assert(doc.insertNode(sample));
                    const auto with_sample = *source::encodeMaterialSource(doc.source());
                    auto rejected = doc.setTextureSlots(doc.historyView()->history.current, {});
                    assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED);
                    assert(*source::encodeMaterialSource(doc.source()) == with_sample);
                    textures.front().texture = doc.source().id;
                    rejected = doc.setTextureSlots(doc.historyView()->history.current, textures);
                    assert(!rejected && rejected.error().code == editing::EEditError::INVALID_ARGUMENT);
                    assert(textures.front().texture == doc.source().id);
                    std::vector<source::ParamSlotDecl> parameters{
                        {"Factor", source::EValueType::FLOAT, {0.2F, 0, 0, 0}}};
                    assert(doc.setParameterSlots(doc.historyView()->history.current, parameters));
                    auto parameter = std::unique_ptr<source::Node>(new source::ParamNode(source::EValueType::FLOAT));
                    assert(doc.insertNode(parameter));
                    parameters.front().type = source::EValueType::VEC3;
                    rejected = doc.setParameterSlots(doc.historyView()->history.current, parameters);
                    assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED);
                    while (doc.historyView()->history.current != before.current)
                    {
                        assert(doc.undo());
                    }
                    assert(*source::encodeMaterialSource(doc.source()) == original);
                    std::puts("material slots: journaled binding changes, referenced removal/type rejection, wrong "
                              "asset retained, exact undo");
                }
                bool reentered = false;
                auto observed = doc.observeScoped<mat::MaterialEditor::contentChanged>(
                    [&](editing::Revision revision) noexcept
                    {
                        assert(revision == doc.historyView()->history.revision);
                        auto rejected = doc.rename("reentry");
                        assert(!rejected && rejected.error().code == editing::EEditError::BUSY);
                        reentered = true;
                    });
                assert(doc.setConstant(node, {0.5F, 0.25F, 0.125F, 0}));
                assert(reentered);
                auto invalid = doc.setConstant(lux::graph::NodeId{9999}, {});
                assert(!invalid && invalid.error().code == editing::EEditError::PRECONDITION_FAILED);
                assert(doc.undo() && doc.historyView()->history.clean);
                assert(doc.redo() && !doc.historyView()->history.clean);
                assert(doc.rename("Saved S1"));
                saved_ = doc.historyView()->history.current;
                save_ = *doc.requestSave("test");
                assert(doc.rename("Edited S2"));
                compile_ = *doc.requestCompile();
                assert(doc.rename("Edited S3"));
                stage_ = 1;
            }
            else if (stage_ == 3)
            {
                assert(doc.source().name == "Saved S1" && doc.historyView()->history.clean);
                assert(doc.source().graph.nodes().size() == 2);
                compile_ = *doc.requestCompile();
                stage_ = 4;
            }
            else
            {
                before_ = doc.historyView()->history;
                compile_ = *doc.requestCompile();
                stage_ = 7;
            }
        }
        else if (stage_ == 1)
        {
            auto &doc = document();
            auto save = doc.saveStatus(save_);
            auto compile = doc.compileStatus(compile_);
            assert(save && compile);
            if (auto *failed = std::get_if<SaveRetryable>(&*save))
            {
                std::printf("SAVE ERROR %s:%llu %s\n", failed->failure.domain.c_str(), failed->failure.reason,
                            failed->failure.message.c_str());
                assert(false);
            }
            if (std::holds_alternative<SavePending>(*save) ||
                std::holds_alternative<mat::MaterialCompilePending>(*compile))
            {
                return;
            }
            assert(std::holds_alternative<SaveSucceeded>(*save));
            assert(std::get<SaveSucceeded>(*save).captured == saved_ && !doc.historyView()->history.clean);
            if (auto *failed = std::get_if<mat::MaterialCompileFailed>(&*compile))
            {
                std::printf("COMPILE ERROR %s:%llu %s\n", failed->failure.domain.c_str(), failed->failure.reason,
                            failed->failure.message.c_str());
            }
            assert(std::holds_alternative<mat::MaterialCompileSucceeded>(*compile));
            assert(!std::get<mat::MaterialCompileSucceeded>(*compile).current);
            auto stale = doc.compiled(compile_);
            assert(!stale && stale.error().code == EEditorError::STALE_REQUEST);
            assert(doc.acknowledgeSave(save_) && doc.acknowledgeCompile(compile_));
            doc.requestClose();
            stage_ = 2;
        }
        else if (stage_ == 2 || stage_ == 5)
        {
            if (editor_->document(document_))
            {
                return;
            }
            open(stage_ == 2 ? identity(2) : identity(3));
            ++stage_;
        }
        else if (stage_ == 4)
        {
            auto &doc = document();
            auto status = doc.compileStatus(compile_);
            assert(status);
            if (std::holds_alternative<mat::MaterialCompilePending>(*status))
            {
                return;
            }
            auto compiled = doc.compiled(compile_);
            assert(compiled && !compiled->get().gbuffer_spirv.empty() && !compiled->get().forward_spirv.empty());
            std::printf("compiled actual SPIR-V: gbuffer=%zu forward=%zu\n", compiled->get().gbuffer_spirv.size(),
                        compiled->get().forward_spirv.size());
            auto published = doc.requestPublish(compile_, "test");
            assert(published);
            save_ = *published;
            assert(doc.acknowledgeCompile(compile_));
            stage_ = 8;
        }
        else if (stage_ == 8)
        {
            auto &doc = document();
            auto status = doc.saveStatus(save_);
            assert(status);
            if (const auto *failure = std::get_if<SaveRetryable>(&*status))
            {
                std::printf("PUBLISH ERROR %s:%llu %s\n", failure->failure.domain.c_str(), failure->failure.reason,
                            failure->failure.message.c_str());
                assert(false);
            }
            if (std::holds_alternative<SavePending>(*status))
            {
                return;
            }
            assert(std::holds_alternative<SaveSucceeded>(*status));
            const auto *entry = project_->asset(identity(2));
            assert(entry && entry->source_digest == entry->compiled_source_digest && !entry->cooked_path.empty());
            assert(project_->assets().resolve("/Project/Materials/Main") == identity(2));
            auto blob = project_->assets().open(identity(2));
            assert(blob);
            auto decoded = lux::asset::TAssetSerDeser<lux::asset::MaterialAsset>::decode(
                identity(2), blob->bytes, lux::asset::AssetDecodeLimits{256U << 20, 256U << 20, 64});
            assert(decoded && !(*decoded)->data().forward_spirv.empty());
            assert(project_->catalogAsset(identity(2))->magic == lux::asset::MaterialAsset::primary_magic);
            assert(doc.acknowledgeSave(save_));
            doc.requestClose();
            stage_ = 5;
            std::puts("compiled Material published with fixed source; actual Project VFS resolve/open/decode succeeds");
        }
        else if (stage_ == 7)
        {
            auto &doc = document();
            auto status = doc.compileStatus(compile_);
            assert(status);
            if (std::holds_alternative<mat::MaterialCompilePending>(*status))
            {
                return;
            }
            assert(std::holds_alternative<mat::MaterialCompileFailed>(*status));
            const auto &failure = std::get<mat::MaterialCompileFailed>(*status).failure;
            const auto *exact = std::any_cast<source::MaterialCompileFailure>(&failure.cause);
            assert(failure.domain == "material.compile" && exact &&
                   exact->code == source::EMaterialCompileError::MISSING_REQUIRED_OUTPUT);
            const auto after = doc.historyView()->history;
            assert(after.current == before_.current && after.revision == before_.revision && after.clean);
            std::printf("unfinished graph compile rejection: domain=%s reason=%llu history unchanged\n",
                        failure.domain.c_str(), failure.reason);
            assert(doc.acknowledgeCompile(compile_));
            compile_ = *doc.requestCompile();
            doc.requestClose();
            evidence_.completed = true;
            exit_.request(*editor_);
        }
    }
    void draw(Editor &, PollBudget &) override {}
    void wait() override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void stopPresenting() noexcept override {}
    void requestClose() noexcept override
    {
        evidence_.closed = true;
    }
    CloseStatus closeStatus() const override
    {
        return {evidence_.closed ? ECloseState::CLOSED : ECloseState::OPEN, {}};
    }

  private:
    mat::MaterialEditor &document()
    {
        auto document = editor_->document(document_);
        assert(document);
        auto *typed = dynamic_cast<mat::MaterialEditor *>(&document->get());
        assert(typed);
        return *typed;
    }
    void open(lux::asset::AssetId source)
    {
        auto request =
            editor_->requestOpen({{project_->manifest().id, source, std::string(mat::kMaterialDocumentType)}, "test"});
        assert(request);
        open_ = *request;
    }
    Evidence &evidence_;
    Editor *editor_{};
    Project *project_{};
    lux::process::ExecutionRuntime *runtime_{};
    OpenRequestId open_;
    DocumentHandle document_;
    SaveRequestId save_;
    mat::MaterialCompileId compile_;
    editing::StateId saved_;
    editing::HistorySnapshot before_;
    unsigned stage_{};
    std::chrono::steady_clock::time_point began_{std::chrono::steady_clock::now()};
};
int main(int argc, char **argv)
{
    assert(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "verify"));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3)
    {
        const std::filesystem::path root{argv[1]};
        auto project = readProjectSource(root / "Project.luxproject");
        assert(project && !project->mounts.empty());
        std::ifstream input(root / "Material.luxmaterial", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
        auto decoded = source::decodeMaterialSource(bytes);
        assert(decoded && decoded->id == identity(2) && decoded->name == "Saved S1");
        assert(decoded->graph.nodes().size() == 2);
        const auto entry = std::ranges::find(project->manifest.assets, identity(2), &ProjectAssetEntry::id);
        assert(entry != project->manifest.assets.end());
        assert(entry->source_digest == *projectFileDigest(root / "Material.luxmaterial"));
        assert(entry->source_digest == entry->compiled_source_digest && !entry->cooked_path.empty());
        std::puts("PASS new-process Material reopen: actual codec, Saved S1, graph, identity, source/artifact digests "
                  "and package");
        return 0;
    }
    lux::meta::ReflectionRegistry::initRegistry();
    fixture(argv[1]);
    Evidence evidence;
    EditorConfig config;
    config.project_file = std::filesystem::path(argv[1]) / "Project.luxproject";
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.frontend = [&] { return std::make_unique<Probe>(evidence); };
    Editor editor(std::move(config));
    const auto result = editor.exec();
    assert(result == 0 && evidence.completed && evidence.closed);
    std::puts("PASS Material actual codec/open/unique owner/edits/reentry/undo/redo/S1 save while S3 "
              "dirty/reopen/stale compile/actual compiler/close with work; no Window or GPU owner created");
}
