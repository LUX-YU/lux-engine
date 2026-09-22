#include "FiniteCost.hpp"
#include "TestExit.hpp"
#include "flow_metadata.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <thread>
using namespace lux::editor;
namespace flow = lux::editor::flowforge;
namespace source = lux::flowforge;
constexpr auto linker = D2_FLOW_LINKER;
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
    source::FlowGraph graph;
    const auto index = graph.addNodes(std::make_unique<source::OnEventNode>("tick"));
    const auto node = graph.getNode(index).node->id();
    assert(graph.addExport({source::FlowForgeExportNodeId{1}, node, 0x1234}));
    auto owner = std::make_shared<MetadataOwner>();
    static_cast<void>(
        graph.addNodes(std::make_unique<source::GetFieldNode>(owner->record, owner->record.fields.front())));
    auto document = source::captureFlowSource(identity(2), "Flow source", graph);
    assert(document);
    auto encoded = source::encodeFlowSource(*document);
    assert(encoded);
    write(root / "Logic.luxflow", *encoded);
    ProjectManifest project{identity(1),
                            "FlowForge protocol",
                            {},
                            {{identity(2),
                              EProjectAssetKind::FLOW_GRAPH,
                              "Logic.luxflow",
                              {},
                              projectContentDigest(std::as_bytes(std::span{encoded->data(), encoded->size()})),
                              {},
                              "Scripts/Main"}}};
    auto manifest = encodeProjectManifest(project);
    assert(manifest);
    write(root / "Project.luxproject", *manifest);
}
struct Evidence final
{
    bool completed{};
    std::weak_ptr<const void> metadata;
};
class CompileReceiver final : public lux::object::Object<CompileReceiver>
{
  public:
    explicit CompileReceiver(lux::object::ObjectDispatcherRef dispatcher) : Object(dispatcher)
    {
    }
    void completed(const flow::FlowCompileId &id) noexcept
    {
        values.push_back(id);
    }
    std::vector<flow::FlowCompileId> values;
};

class Probe final
{
    TestExit exit_;

  public:
    explicit Probe(Evidence &evidence) : evidence_(evidence)
    {
    }
    ~Probe()
    {
        assert(stdexec::sync_wait(startup_.close()));
    }

    void bind(Editor &editor) noexcept
    {
        editor_ = &editor;
    }

    EditorResult<DocumentRegistration> registration(lux::process::ExecutionRuntime &runtime)
    {
        auto work = stdexec::then(stdexec::schedule(runtime.main()), [this]() noexcept {
            auto requested = editor_->requestOpen(
                {{identity(1), identity(2), std::string(flow::kFlowForgeDocumentType)}, "test-start"});
            assert(requested);
            initial_ = *requested;
        });
        auto errors = stdexec::upon_error(std::move(work), [](lux::process::EExecutionError) noexcept {
            assert(false && "Initial test action was not admitted to Main");
        });
        assert(startup_.start(std::move(errors)));
        auto owner = std::make_shared<MetadataOwner>();
        evidence_.metadata = owner;
        auto environment = flowMetadata(owner);
        assert(source::validateFlowSourceEnvironment(environment));
        return DocumentRegistration{
            std::string(flow::kFlowForgeDocumentType),
            [this, &runtime, environment](Project &project, const OpenDocumentRequest &request) {
                if (!project_)
                {
                    project_ = &project;
                    source::FlowSourceEnvironment invalid;
                    const lux::meta::RefFunction *null_function = nullptr;
                    invalid.functions = {&null_function, 1};
                    auto refused = flow::openFlowForgeDocument(project, request, runtime, invalid);
                    assert(!refused && refused.error().domain == "flowforge.metadata" &&
                           refused.error().reason ==
                               static_cast<std::uint64_t>(source::EFlowSourceError::UNKNOWN_REFLECTION_MEMBER));
                    open();
                    enqueue();
                }
                return flow::openFlowForgeDocument(project, request, runtime, environment);
            }};
    }

    void poll(PollBudget &)
    {
        exit_.poll();
        if (initial_.value)
        {
            auto initial = editor_->openStatus(initial_);
            assert(initial);
            if (!std::holds_alternative<OpenPending>(*initial))
            {
                assert(editor_->acknowledgeOpen(initial_));
                initial_ = {};
            }
        }
        assert(std::chrono::steady_clock::now() - began_ < std::chrono::seconds(60));
        if (!project_ || evidence_.completed)
        {
            return;
        }
        if (stage_ == 0 || stage_ == 4)
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
            first_notice_ = doc.observeScoped<flow::FlowForgeEditor::compileFinished>(
                [this, &doc](const flow::FlowCompileId &id) noexcept {
                    notified_ = id;
                    if (stage_ != 6)
                    {
                        expected_notices_.push_back(id);
                    }
                    const auto acknowledgment = doc.acknowledgeCompile(id);
                    assert(!acknowledgment && acknowledgment.error().code == EEditorError::BUSY);
                    if (stage_ == 6)
                    {
                        doc.requestClose();
                        assert(editor_->document(document_) && doc.compileStatus(id));
                    }
                    ++first_notices_;
                });
            second_notice_ = doc.observeScoped<flow::FlowForgeEditor::compileFinished>(
                [this, &doc](const flow::FlowCompileId &id) noexcept {
                    assert(id == notified_ && doc.compileStatus(id));
                    ++second_notices_;
                });
            auto queued = doc.observe<flow::FlowForgeEditor::compileFinished, &CompileReceiver::completed,
                                      lux::object::EDelivery::QUEUED>(receiver_);
            assert(queued);
            queued_notice_ = lux::object::ScopedConnection{std::move(*queued)};
            if (stage_ == 0)
            {
                auto shared = editor_->requestOpen({doc.summary().key, "duplicate"});
                assert(shared && std::get<DocumentHandle>(*editor_->openStatus(*shared)) == document_);
                assert(editor_->acknowledgeOpen(*shared));
                const auto original = doc.capture();
                assert(original && doc.views().empty() && doc.historyView()->history.clean);
                measureEdits("Flow.node-layout", doc, [&](unsigned index) {
                    const std::array moved{
                        lux::graph::GraphLayoutEntry{lux::graph::NodeId{original->nodes.front().id.value},
                                                     {static_cast<float>(index + 1), 32, true}}};
                    return doc.moveNodes(moved);
                });
                assert(!evidence_.metadata.expired() && doc.metadata().classes.size() == 1);
                {
                    auto foreign = std::make_shared<MetadataOwner>();
                    std::unique_ptr<source::Node> input =
                        std::make_unique<source::NativeFuncCall>(0, foreign->function);
                    const auto *retained = input.get();
                    const auto before = doc.historyView()->history;
                    auto refused = doc.insertNode(input);
                    assert(!refused && refused.error().code == editing::EEditError::PRECONDITION_FAILED &&
                           refused.error().domain_code ==
                               static_cast<std::uint64_t>(source::EFlowSourceError::UNKNOWN_REFLECTION_MEMBER));
                    assert(input.get() == retained && *doc.capture() == *original &&
                           doc.historyView()->history.current == before.current);
                    input = std::make_unique<source::NativeFuncCall>(0, *doc.metadata().functions.front());
                    auto inserted = doc.insertNode(input);
                    assert(inserted && !input);
                    const auto after = *doc.capture();
                    assert(doc.undo() && *doc.capture() == *original && doc.redo() && *doc.capture() == after);
                    assert(doc.undo() && *doc.capture() == *original);
                    auto duplicate = doc.metadata();
                    const auto *function = duplicate.functions.front();
                    const std::array functions{function, function};
                    duplicate.functions = functions;
                    auto invalid = source::validateFlowSourceEnvironment(duplicate);
                    assert(!invalid && invalid.error().code == source::EFlowSourceError::INVALID_IDENTITY);
                    std::puts("metadata: caller released; exact unregistered-pointer and duplicate registration "
                              "failures; native node Undo/Redo retained source");
                }
                {
                    const auto initial = doc.historyView()->history.current;
                    const auto type = lux::meta::builtin_ref_type_ptr<std::int32_t>();
                    std::unique_ptr<source::Node> definition = std::make_unique<source::FuncDefNode>(
                        0, "Compute", std::vector<source::FuncArgInfo>{{type, "input"}},
                        std::vector<source::FuncArgInfo>{{type, "output"}});
                    const auto function = doc.insertNode(definition);
                    assert(function && !definition);
                    const auto call = doc.insertFunctionUse(*function, false);
                    const auto returned = doc.insertFunctionUse(*function, true);
                    assert(call && returned);
                    const auto def_source = *doc.captureNode(*function);
                    const auto return_source = *doc.captureNode(*returned);
                    const auto call_source = *doc.captureNode(*call);
                    const auto data_input =
                        std::ranges::find(call_source.inputs, source::EPinKind::DATA_IN, &source::FlowSourcePin::kind)
                            ->id;
                    assert(doc.setPinLiteral(data_input, {source::EFlowLiteralKind::SIGNED, "42"}));
                    assert(doc.connect(def_source.outputs.front().id, return_source.inputs.front().id));
                    const auto argument =
                        std::ranges::find(def_source.outputs, source::EPinKind::DATA_OUT, &source::FlowSourcePin::kind)
                            ->id;
                    const auto result_input =
                        std::ranges::find(return_source.inputs, source::EPinKind::DATA_IN, &source::FlowSourcePin::kind)
                            ->id;
                    assert(doc.connect(argument, result_input));
                    const auto before = *doc.capture();
                    const auto before_state = doc.historyView()->history.current;
                    auto signature = std::get<source::FlowSourceSignature>(def_source.parameters);
                    signature.arguments.push_back({"extra", std::string(type->name)});
                    auto applied = doc.setFunctionSignature(before_state, *function, "ComputeMore", signature);
                    if (!applied)
                    {
                        std::printf("signature failure code=%u domain=%llu\n", unsigned(applied.error().code),
                                    applied.error().domain_code);
                    }
                    assert(applied);
                    const auto after = *doc.capture();
                    const auto changed = *doc.captureNode(*function);
                    const auto changed_call = *doc.captureNode(*call);
                    assert(changed.outputs.size() == def_source.outputs.size() + 1);
                    assert(changed.outputs[1].id == argument && changed_call.inputs[1].id == data_input);
                    assert(doc.pinLiteral(data_input)->value == "42");
                    const auto new_pin = changed.outputs.back().id;
                    assert(new_pin.valid() && doc.undo() && *doc.capture() == before);
                    assert(doc.redo() && *doc.capture() == after &&
                           doc.captureNode(*function)->outputs.back().id == new_pin);
                    const auto current = doc.historyView()->history;
                    auto stale = doc.setFunctionSignature(before_state, *function, "stale", signature);
                    assert(!stale && stale.error().code == editing::EEditError::STALE_BASE);
                    auto removed_argument = signature;
                    removed_argument.arguments.clear();
                    const auto rejected = doc.setFunctionSignature(current.current, *function, "bad", removed_argument);
                    assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED &&
                           rejected.error().domain_code ==
                               static_cast<std::uint64_t>(lux::graph::EGraphTopologyError::UNKNOWN_PIN));
                    assert(*doc.capture() == after && doc.historyView()->history.current == current.current);
                    assert(doc.setFunctionSignature(current.current, *function, "ComputeMore", signature));
                    assert(doc.historyView()->history.current == current.current);
                    assert(!doc.removeNodes({&*function, 1}));
                    std::size_t undos{};
                    while (doc.historyView()->history.current != initial)
                    {
                        assert(++undos < 20 && doc.undo());
                    }
                    assert(*doc.capture() == *original);
                    auto event_source = *doc.captureNode(original->nodes.front().id);
                    auto event_signature = std::get<source::FlowSourceSignature>(event_source.parameters);
                    event_signature.arguments.push_back({"tick", std::string(type->name)});
                    assert(doc.setFunctionSignature(initial, event_source.id, "RenamedEvent", event_signature));
                    assert(doc.exports().size() == original->exports.size() && doc.undo() &&
                           *doc.capture() == *original);
                    std::puts("function signatures: atomic entry/caller/return replacement, stable pins and defaults, "
                              "exact connected removal/stale rejection, event export retained, full undo/redo");
                }
                const auto invalid_variable =
                    doc.addVariable("bad", "Unknown.Type", {source::EFlowLiteralKind::REAL, "1"});
                assert(!invalid_variable && doc.variables().empty() && doc.historyView()->history.clean);
                const auto bool_type = std::string(lux::meta::builtin_ref_type_ptr<bool>()->name);
                const auto real_type = std::string(lux::meta::builtin_ref_type_ptr<double>()->name);
                const auto variable =
                    doc.addVariable("enabled", bool_type, {source::EFlowLiteralKind::BOOLEAN, "true"});
                const auto other = doc.addVariable("gain", real_type, {source::EFlowLiteralKind::REAL, "2.5"});
                assert(variable && other && *other > *variable && doc.variables().size() == 2);
                const auto with_variables = *doc.capture();
                assert(!doc.addVariable("enabled", bool_type, {source::EFlowLiteralKind::BOOLEAN, "false"}));
                auto value = *source::captureFlowVariable(doc.variables().front());
                value.name = "active";
                value.value.value = "false";
                assert(doc.setVariable(value) && doc.variables().front().name == "active");
                assert(doc.undo() && *doc.capture() == with_variables);
                auto new_type = *source::captureFlowVariable(doc.variables().front());
                new_type.type = real_type;
                new_type.value = {source::EFlowLiteralKind::REAL, "0.75"};
                assert(doc.setVariable(new_type));
                assert(*doc.variables().front().type == *lux::meta::builtin_ref_type_ptr<double>());
                assert(doc.undo() && *doc.capture() == with_variables);
                assert(doc.removeVariable(*variable) && doc.variables().front().id == *other);
                assert(doc.undo() && *doc.capture() == with_variables);
                std::unique_ptr<source::Node> getter = std::make_unique<source::GetVariableNode>(
                    *variable, source::DataPinInfo{"enabled", lux::meta::builtin_ref_type_ptr<bool>()});
                auto getter_id = doc.insertNode(getter);
                if (!getter_id)
                {
                    const auto *supplied =
                        static_cast<const source::DataOutPin *>(getter->outPins().front())->info().type;
                    std::printf("getter rejected code=%u domain=%llu equal-metadata=%d same-pointer=%d\n",
                                unsigned(getter_id.error().code), getter_id.error().domain_code,
                                *supplied == *doc.variables().front().type, supplied == doc.variables().front().type);
                }
                assert(getter_id && !getter);
                const auto before_rejected = *doc.capture();
                assert(!doc.removeVariable(*variable));
                value.type = real_type;
                value.value = {source::EFlowLiteralKind::REAL, "1"};
                assert(!doc.setVariable(value) && *doc.capture() == before_rejected);
                assert(doc.undo()); // Getter goes away before its variable.
                assert(doc.undo() && doc.variables().size() == 1);
                assert(doc.undo() && *doc.capture() == *original);
                const auto branch_variable =
                    doc.addVariable("branch", bool_type, {source::EFlowLiteralKind::BOOLEAN, "true"});
                assert(branch_variable && *branch_variable > *other && doc.undo() && *doc.capture() == *original);
                std::puts("variables: typed creation/update/delete/order replay, referenced delete/type rejection, "
                          "branch IDs monotonic; author graph preserved");
                source::FuncDefNode foreign_definition(1, "foreign", std::vector<source::FuncArgInfo>{});
                std::unique_ptr<source::Node> foreign_call =
                    std::make_unique<source::GraphFuncCallNode>(0, foreign_definition);
                auto *foreign_pointer = foreign_call.get();
                auto foreign_result = doc.insertNode(foreign_call);
                assert(!foreign_result && foreign_call.get() == foreign_pointer && *doc.capture() == *original);
                const auto event = original->nodes.front().id;
                auto added = std::unique_ptr<source::Node>(new source::BranchNode(0));
                auto *pointer = added.get();
                assert(!doc.insertNode(added, {std::numeric_limits<float>::quiet_NaN(), 0, true}));
                assert(added.get() == pointer && doc.historyView()->history.clean);
                const auto created = doc.insertNode(added, {100, 120, true});
                assert(created && !added);
                auto snapshot = doc.capture();
                assert(snapshot);
                const auto branch = std::ranges::find(snapshot->nodes, *created, &source::FlowSourceNode::id);
                const auto from = original->nodes.front().outputs.front().id;
                const auto to = branch->inputs.front().id;
                const auto condition =
                    std::ranges::find(branch->inputs, source::EPinKind::DATA_IN, &source::FlowSourcePin::kind)->id;
                assert(doc.pinLiteral(condition)->value == "false");
                const auto untouched = doc.historyView()->history;
                assert(!doc.setPinLiteral(condition, {source::EFlowLiteralKind::BOOLEAN, "not-a-bool"}));
                assert(doc.historyView()->history.current == untouched.current &&
                       doc.pinLiteral(condition)->value == "false");
                assert(doc.setPinLiteral(condition, {source::EFlowLiteralKind::BOOLEAN, "true"}));
                assert(doc.pinLiteral(condition)->value == "true" && doc.undo() &&
                       doc.pinLiteral(condition)->value == "false");
                assert(doc.connect(from, to));
                const auto connected = doc.capture();
                assert(connected);
                const auto before = doc.historyView()->history;
                assert(!doc.connect(lux::graph::PinId{99999}, to));
                assert(doc.historyView()->history.current == before.current &&
                       doc.historyView()->history.revision == before.revision);
                assert(!doc.removeNodes({&event, 1})); // An exported entry cannot be removed silently.
                assert(doc.removeNodes({&*created, 1}) && doc.nodes().size() == 2);
                assert(doc.undo() && *doc.capture() == *connected);
                assert(doc.redo() && doc.undo() && *doc.capture() == *connected);
                assert(doc.undo() && doc.undo() && *doc.capture() == *original);
                assert(doc.setExports({{source::FlowForgeExportNodeId{1}, event, 0x2345}}));
                assert(doc.undo() && *doc.capture() == *original);
                bool entered{};
                auto connection =
                    doc.observeScoped<flow::FlowForgeEditor::contentChanged>([&](editing::Revision revision) noexcept {
                        assert(revision == doc.historyView()->history.revision);
                        const auto reentry = doc.rename("wrong");
                        assert(!reentry && reentry.error().code == editing::EEditError::BUSY);
                        entered = true;
                    });
                assert(doc.rename("Saved S1") && entered);
                saved_ = doc.historyView()->history.current;
                auto save = doc.requestSave("test");
                assert(save);
                save_ = *save;
                assert(doc.rename("Edited S2"));
                auto compile = doc.requestCompile("missing-d2-test-linker.exe");
                assert(compile);
                compile_ = *compile;
                std::puts("real FlowForge owner: unique open, graph/exports history, failed edit preservation, "
                          "synchronous observer gate passed");
                stage_ = 1;
            }
            else
            {
                assert(doc.summary().title == "Saved S1" && doc.historyView()->history.clean &&
                       doc.nodes().size() == 2);
                compile_ = *doc.requestCompile(linker);
                stage_ = 5;
            }
        }
        else if (stage_ == 1)
        {
            auto &doc = document();
            auto save = doc.saveStatus(save_);
            auto compile = doc.compileStatus(compile_);
            assert(save && compile);
            if (const auto *failure = std::get_if<SaveRetryable>(&*save))
            {
                std::printf("SAVE ERROR %s:%llu %s\n", failure->failure.domain.c_str(), failure->failure.reason,
                            failure->failure.message.c_str());
                assert(false);
            }
            if (std::holds_alternative<SavePending>(*save) ||
                std::holds_alternative<flow::FlowCompilePending>(*compile))
            {
                return;
            }
            const auto *failure = std::get_if<flow::FlowCompileFailed>(&*compile);
            assert(failure && failure->retryable && failure->failure.domain == "flowforge.link");
            const auto *cause = std::any_cast<source::FlowForgeFailure>(&failure->failure.cause);
            assert(cause && cause->code == source::EFlowForgeError::LINK_FAILED);
            assert(std::holds_alternative<SaveSucceeded>(*save));
            assert(doc.historyView()->history.saved == saved_ && !doc.historyView()->history.clean);
            assert(doc.acknowledgeSave(save_));
            assert(doc.rename("Edited S3"));
            assert(doc.retryLink(compile_, linker));
            std::puts("fixed S1 saved while S2 dirty; exact LINK_FAILED retained, same compile request retried after "
                      "S3 edit");
            stage_ = 2;
        }
        else if (stage_ == 2 || stage_ == 5)
        {
            auto &doc = document();
            auto status = doc.compileStatus(compile_);
            assert(status);
            if (std::holds_alternative<flow::FlowCompilePending>(*status))
            {
                return;
            }
            if (const auto *failure = std::get_if<flow::FlowCompileFailed>(&*status))
            {
                std::printf("COMPILE ERROR %s:%llu %s\n", failure->failure.domain.c_str(), failure->failure.reason,
                            failure->failure.message.c_str());
            }
            assert(std::holds_alternative<flow::FlowCompileSucceeded>(*status));
            if (stage_ == 2)
            {
                auto stale = doc.compiled(compile_);
                assert(!stale && stale.error().code == EEditorError::STALE_REQUEST);
                acknowledge(doc);
                doc.requestClose();
                stage_ = 3;
            }
            else
            {
                auto artifact = doc.compiled(compile_);
                assert(artifact && !artifact->get().payload().empty());
                auto module = lux::script::loadNativeModule(artifact->get().payload(), "flow_protocol");
                assert(module && module->findFunction(0x1234));
                lux_script_native_instance_context instance{};
                lux_script_call_frame frame{};
                assert(module->findFunction(0x1234)->invoke(&instance, &frame) == 0);
                std::printf("reopened S1 native artifact=%zu bytes; actual exported Tick invocation=0\n",
                            artifact->get().payload().size());
                auto published = doc.requestPublish(compile_, "test");
                assert(published);
                save_ = *published;
                saved_ = doc.historyView()->history.current;
                acknowledge(doc); // Save owns its byte captures after compile retirement.
                assert(doc.rename("Source newer than artifact"));
                stage_ = 7;
            }
        }
        else if (stage_ == 7 || stage_ == 8)
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
            assert(entry && !entry->cooked_path.empty());
            assert(project_->assets().resolve("/Project/Scripts/Main") == identity(2));
            auto blob = project_->assets().open(identity(2));
            assert(blob);
            auto decoded = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
                identity(2), blob->bytes, lux::asset::AssetDecodeLimits{256U << 20, 256U << 20, 64});
            assert(decoded && (*decoded)->data().findExport(lux::script::ScriptSymbolId{0x1234}));
            if (stage_ == 7)
            {
                assert(entry->source_digest == entry->compiled_source_digest);
                assert(doc.historyView()->history.saved == saved_ && !doc.historyView()->history.clean);
                compiled_digest_ = entry->compiled_source_digest;
                assert(doc.acknowledgeSave(save_));
                save_ = *doc.requestSave("source-only");
                stage_ = 8;
                std::puts(
                    "published fixed source + script pak; Main catalog/VFS adopted; newer author edit remains dirty");
            }
            else
            {
                assert(entry->compiled_source_digest == compiled_digest_ && entry->source_digest != compiled_digest_);
                assert(doc.historyView()->history.clean && doc.acknowledgeSave(save_));
                queued_notice_.reset();
                assert(doc.requestCompile(linker));
                doc.requestClose();
                stage_ = 6;
                std::puts(
                    "source-only save preserved older artifact and compiled-source digest; VFS bytes remain decodable");
            }
        }
        else if (stage_ == 3 || stage_ == 6)
        {
            if (editor_->document(document_))
            {
                return;
            }
            if (stage_ == 3)
            {
                open();
                stage_ = 4;
            }
            else
            {
                assert(first_notices_ >= 3 && first_notices_ == second_notices_);
                // A late drain after disconnect and owner retirement cannot target
                // the next document incarnation.
                assert(observer_queue_.dispatchPending(64) == 0);
                assert(receiver_.values == expected_notices_);
                evidence_.completed = true;
                exit_.request(*editor_);
            }
        }
    }

  private:
    // Test actions run at the real Main dispatcher boundary, never inside a
    // widget draw or a replacement Editor loop. Reentrant posts run next turn.
    void enqueue()
    {
        const auto posted =
            lux::object::detail::post(project_->dispatcherRef(), lux::object::detail::makeMessage([this]() noexcept {
                                          if (editor_->closing())
                                          {
                                              return;
                                          }
                                          PollBudget budget;
                                          poll(budget);
                                          if (!editor_->closing())
                                          {
                                              enqueue();
                                          }
                                      }));
        assert(posted == lux::object::detail::EPostStatus::POSTED);
    }

    void acknowledge(flow::FlowForgeEditor &doc)
    {
        assert(doc.acknowledgeCompile(compile_));
        const auto duplicate = doc.acknowledgeCompile(compile_);
        assert(!duplicate && duplicate.error().code == EEditorError::STALE_REQUEST);
        static_cast<void>(observer_queue_.dispatchPending(expected_notices_.size()));
        assert(receiver_.values == expected_notices_);
    }

    flow::FlowForgeEditor &document()
    {
        auto value = editor_->document(document_);
        assert(value);
        auto *typed = dynamic_cast<flow::FlowForgeEditor *>(&value->get());
        assert(typed);
        return *typed;
    }
    void open()
    {
        auto request = editor_->requestOpen(
            {{project_->manifest().id, identity(2), std::string(flow::kFlowForgeDocumentType)}, "test"});
        assert(request);
        open_ = *request;
    }
    Evidence &evidence_;
    Editor *editor_{};
    Project *project_{};
    lux::process::TaskScope startup_;
    OpenRequestId initial_;
    OpenRequestId open_;
    DocumentHandle document_;
    SaveRequestId save_;
    flow::FlowCompileId compile_;
    editing::StateId saved_;
    std::string compiled_digest_;
    unsigned stage_{};
    std::chrono::steady_clock::time_point began_{std::chrono::steady_clock::now()};
    flow::FlowCompileId notified_;
    unsigned first_notices_{}, second_notices_{};
    std::vector<flow::FlowCompileId> expected_notices_;
    lux::object::ObjectMessageQueue observer_queue_;
    CompileReceiver receiver_{observer_queue_.dispatcherRef()};
    lux::object::ScopedConnection first_notice_, second_notice_;
    lux::object::ScopedConnection queued_notice_;
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
        std::ifstream input(root / "Logic.luxflow", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
        auto decoded = source::decodeFlowSource(bytes);
        assert(decoded && decoded->id == identity(2) && decoded->name == "Source newer than artifact");
        assert(!decoded->nodes.empty() && decoded->exports.size() == 1);
        const auto entry = std::ranges::find(project->manifest.assets, identity(2), &ProjectAssetEntry::id);
        assert(entry != project->manifest.assets.end());
        assert(entry->source_digest == *projectFileDigest(root / "Logic.luxflow"));
        assert(entry->source_digest != entry->compiled_source_digest && !entry->cooked_path.empty());
        std::puts(
            "PASS new-process Flow reopen: actual codec, newer source, identity, exports and retained older artifact");
        return 0;
    }
    lux::meta::ReflectionRegistry::initRegistry();
    fixture(argv[1]);
    Evidence evidence;
    Probe probe(evidence);
    EditorConfig config;
    config.project_file = std::filesystem::path(argv[1]) / "Project.luxproject";
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.window.visible = false;
    config.providers.push_back(
        {std::string(flow::kFlowForgeDocumentType),
         [](const ProjectAssetEntry &entry) { return entry.kind == EProjectAssetKind::FLOW_GRAPH; },
         [&probe](auto &runtime, auto &) { return probe.registration(runtime); },
         [](auto &, auto &, auto &, auto &) -> EditorResult<void> { return {}; }});
    {
        Editor editor(std::move(config));
        probe.bind(editor);
        const auto result = editor.exec();
        assert(result == 0 && evidence.completed && editor.documents().empty());
    }
    assert(evidence.metadata.expired());
    auto reopened = readProjectSource(std::filesystem::path(argv[1]) / "Project.luxproject");
    assert(reopened && reopened->mounts.size() == 1);
    assert(reopened->mounts.front().mount.provider->open(identity(2)));
    std::puts("PASS actual FlowForge owner/codec/history/S1 save/reopen/retained link retry/stale result/native "
              "invocation/close with active work");
}
