#include "../TestExit.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <thread>

using namespace lux::editor;
namespace material = lux::editor::material;

namespace
{
    lux::asset::AssetId identity(unsigned char value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }

    void write(const std::filesystem::path &path, std::string_view bytes)
    {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(output.good());
    }

    void createProject(const std::filesystem::path &root)
    {
        assert(!std::filesystem::exists(root));
        std::filesystem::create_directories(root);
        // An actual source graph with no output produces a normal compiler failure.
        // Completion lifetime must be correct for errors as well as successful SPIR-V.
        lux::material::MaterialSourceDocument source{identity(2), "Review material", {}};
        const auto encoded = lux::material::encodeMaterialSource(source);
        assert(encoded);
        write(root / "Material.luxmaterial", *encoded);

        source.id = identity(3);
        const auto other = lux::material::encodeMaterialSource(source);
        assert(other);
        write(root / "Other.luxmaterial", *other);

        ProjectManifest project{identity(1),
                                "D2-C review",
                                {},
                                {{identity(2),
                                  EProjectAssetKind::MATERIAL_GRAPH,
                                  "Material.luxmaterial",
                                  {},
                                  projectContentDigest(std::as_bytes(std::span{encoded->data(), encoded->size()})),
                                  {},
                                  "Material"}}};
        project.assets.push_back({identity(3),
                                  EProjectAssetKind::MATERIAL_GRAPH,
                                  "Other.luxmaterial",
                                  {},
                                  projectContentDigest(std::as_bytes(std::span{other->data(), other->size()})),
                                  {},
                                  "Other"});
        const auto manifest = encodeProjectManifest(project);
        assert(manifest);
        write(root / "Project.luxproject", *manifest);
    }

    struct Evidence final
    {
        std::string mode;
        bool checked{};
        bool correct{};
        bool frontend_closed{};
        unsigned view_close_calls{};
        unsigned view_destructors{};
        unsigned notifications{};
        unsigned second_notifications{};
        unsigned queued_notifications{};
        EEditorError acknowledgment{EEditorError::INVALID_STATE};
        bool acknowledged{};
        material::MaterialCompileId notified;
    };

    class NoticeReceiver final : public lux::object::Object<NoticeReceiver>
    {
      public:
        NoticeReceiver(lux::object::ObjectDispatcherRef dispatcher, Evidence &evidence)
            : Object(dispatcher), evidence_(evidence)
        {
        }
        void completed(const material::MaterialCompileId &id) noexcept
        {
            assert(id == evidence_.notified);
            ++evidence_.queued_notifications;
        }

      private:
        Evidence &evidence_;
    };

    class View final : public DocumentView
    {
      public:
        explicit View(Evidence &evidence) : evidence_(evidence) {}
        ~View() override
        {
            ++evidence_.view_destructors;
        }
        std::string_view id() const noexcept override
        {
            return "review.view";
        }
        void requestClose() noexcept override
        {
            ++evidence_.view_close_calls;
            closing_ = true;
        }
        void poll(PollBudget &) override
        {
            closed_ = closing_;
        }
        CloseStatus closeStatus() const override
        {
            return {closed_ ? ECloseState::CLOSED : closing_ ? ECloseState::CLOSING : ECloseState::OPEN, {}};
        }

      private:
        Evidence &evidence_;
        bool closing_{};
        bool closed_{};
    };

    class Frontend final : public EditorFrontend
    {
        TestExit exit_;

      public:
        explicit Frontend(Evidence &evidence) : evidence_(evidence) {}

        EditorResult<void> beginStartup(Editor &editor, lux::process::ExecutionRuntime &runtime,
                                        lux::object::ObjectDispatcherRef) override
        {
            editor_ = &editor;
            return editor.registerDocument({std::string(material::kMaterialDocumentType),
                                            [this, &runtime](Project &project, const OpenDocumentRequest &request)
                                            {
                                                auto opened = material::openMaterialDocument(project, request, runtime);
                                                if (opened && request.key.source == identity(3))
                                                {
                                                    *opened = std::make_unique<LateOpening>(std::move(*opened),
                                                                                            release_late_);
                                                }
                                                return opened;
                                            }});
        }

        EditorResult<void> enterProject(Editor &editor, Project &, lux::process::ExecutionRuntime &,
                                        lux::object::ObjectDispatcherRef) override
        {
            const auto request = editor.requestOpen(
                {{identity(1), identity(2), std::string(material::kMaterialDocumentType)}, "review"});
            assert(request);
            open_ = *request;
            return {};
        }

        void collectInput(Editor &) override {}
        void poll(PollBudget &) override
        {
            exit_.poll();
            if (evidence_.checked || !open_.value)
            {
                return;
            }
            assert(std::chrono::steady_clock::now() - started_ < std::chrono::seconds(30));

            if (handle_.isNull())
            {
                const auto status = editor_->openStatus(open_);
                assert(status);
                if (std::holds_alternative<OpenPending>(*status))
                {
                    return;
                }
                assert(std::holds_alternative<DocumentHandle>(*status));
                handle_ = std::get<DocumentHandle>(*status);
                assert(editor_->acknowledgeOpen(open_));
                auto &owner = document();
                std::vector<std::unique_ptr<DocumentView>> views;
                views.push_back(std::make_unique<View>(evidence_));
                assert(owner.addViews(views) && views.empty());

                if (evidence_.mode == "exit-review")
                {
                    assert(owner.setShadingModel(lux::rdesc::ELightingTechnique::Unlit));
                    const OpenDocumentRequest late{
                        {identity(1), identity(3), std::string(material::kMaterialDocumentType)}, "late"};
                    const auto first = editor_->requestOpen(late);
                    const auto second = editor_->requestOpen(late);
                    assert(first && second && editor_->cancelOpen(*first));
                    late_ = *second;
                    const auto review = editor_->beginExitReview();
                    assert(review);
                    review_ = *review;
                    const auto denied = editor_->requestOpen(late);
                    assert(!denied && denied.error().code == EEditorError::CLOSING);
                    decisions_ = decisions();
                    const auto pending = editor_->commitExitReview(review_, decisions_);
                    assert(!pending && pending.error().code == EEditorError::BUSY);
                    assert(owner.closeStatus().state == ECloseState::OPEN && evidence_.view_close_calls == 0);
                    release_late_ = true;
                    std::puts(
                        "exit-review: core rejects new open; pending shared opening blocks commit; no close effects");
                    return;
                }

                if (evidence_.mode == "close-intent")
                {
                    owner.requestClose();
                    evidence_.correct = evidence_.view_close_calls == 0 && evidence_.view_destructors == 0;
                    std::printf("close-intent: immediate_view_calls=%u destructors=%u state=%u\n",
                                evidence_.view_close_calls, evidence_.view_destructors,
                                static_cast<unsigned>(owner.closeStatus().state));
                    finish();
                    return;
                }

                connection_ = owner.observeScoped<material::MaterialEditor::compileFinished>(
                    [this](material::MaterialCompileId id) noexcept
                    {
                        evidence_.notified = id;
                        ++evidence_.notifications;
                        const auto result = document().acknowledgeCompile(id);
                        evidence_.acknowledged = result.has_value();
                        if (!result)
                        {
                            evidence_.acknowledgment = result.error().code;
                        }
                        // Do not inspect task-owned storage after the baseline's successful ack.
                    });
                second_connection_ = owner.observeScoped<material::MaterialEditor::compileFinished>(
                    [this](const material::MaterialCompileId &id) noexcept
                    {
                        assert(id == evidence_.notified);
                        ++evidence_.second_notifications;
                    });
                auto queued = owner.observe<material::MaterialEditor::compileFinished, &NoticeReceiver::completed,
                                            lux::object::EDelivery::QUEUED>(receiver_);
                assert(queued);
                queued_connection_ = lux::object::ScopedConnection{std::move(*queued)};
                const auto compile = owner.requestCompile();
                assert(compile);
                compile_ = *compile;
                return;
            }

            if (evidence_.mode == "exit-review")
            {
                advanceReview();
                return;
            }
            if (!evidence_.notifications)
            {
                return;
            }
            evidence_.correct = !evidence_.acknowledged && evidence_.acknowledgment == EEditorError::BUSY &&
                                evidence_.notified == compile_ && evidence_.notifications == 1;
            std::printf("compile-notice: listeners=%u ack_success=%u error=%u identity_match=%u\n",
                        evidence_.notifications, evidence_.acknowledged,
                        static_cast<unsigned>(evidence_.acknowledgment), evidence_.notified == compile_);
            if (!evidence_.acknowledged)
            {
                assert(document().acknowledgeCompile(compile_));
                const auto duplicate = document().acknowledgeCompile(compile_);
                assert(!duplicate && duplicate.error().code == EEditorError::STALE_REQUEST);
            }
            assert(evidence_.queued_notifications == 0);
            assert(observer_queue_.dispatchPending(1) == 1);
            evidence_.correct &= evidence_.second_notifications == 1 && evidence_.queued_notifications == 1;
            std::puts("compile-notice: second DIRECT matched; QUEUED retained identity after outer acknowledgment");
            finish();
        }
        void draw(Editor &, PollBudget &) override {}
        void wait() override
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        void stopPresenting() noexcept override {}
        void requestClose() noexcept override
        {
            evidence_.frontend_closed = true;
        }
        CloseStatus closeStatus() const override
        {
            return {evidence_.frontend_closed ? ECloseState::CLOSED : ECloseState::OPEN, {}};
        }

      private:
        class LateOpening final : public DocumentOpening
        {
          public:
            LateOpening(std::unique_ptr<DocumentOpening> work, bool &released)
                : work_(std::move(work)), released_(released)
            {
            }
            void cancel() noexcept override
            {
                released_ = true;
                work_->cancel();
            }
            void poll() override
            {
                work_->poll();
            }
            bool settled() const noexcept override
            {
                return released_ && work_->settled();
            }
            EditorResult<std::unique_ptr<DocumentEditor>> take() override
            {
                return work_->take();
            }

          private:
            std::unique_ptr<DocumentOpening> work_;
            bool &released_;
        };

        std::vector<DocumentCloseDecision> decisions()
        {
            std::vector<DocumentCloseDecision> result;
            for (const auto &summary : editor_->documents())
            {
                const auto history = editor_->document(summary.handle)->get().reviewClose();
                assert(history);
                result.push_back(
                    {summary.handle, history->current, history->revision, EDocumentCloseDecision::DISCARD_THIS_STATE});
            }
            return result;
        }

        void advanceReview()
        {
            auto &owner = document();
            if (review_stage_ == 0)
            {
                const auto status = editor_->openStatus(late_);
                assert(status);
                if (std::holds_alternative<OpenPending>(*status))
                {
                    return;
                }
                assert(std::holds_alternative<DocumentHandle>(*status));
                const auto late_handle = std::get<DocumentHandle>(*status);
                assert(editor_->acknowledgeOpen(late_));
                auto &late = dynamic_cast<material::MaterialEditor &>(editor_->document(late_handle)->get());
                assert(late.setShadingModel(lux::rdesc::ELightingTechnique::Unlit));
                const auto incomplete = editor_->commitExitReview(review_, decisions_);
                assert(!incomplete && incomplete.error().code == EEditorError::STALE_DOCUMENT);
                decisions_ = decisions();
                assert(owner.undo());
                const auto stale = editor_->commitExitReview(review_, decisions_);
                assert(!stale && stale.error().code == EEditorError::STALE_REQUEST);
                assert(owner.closeStatus().state == ECloseState::OPEN &&
                       late.closeStatus().state == ECloseState::OPEN && evidence_.view_close_calls == 0);
                assert(editor_->cancelExitReview(review_));
                assert(!editor_->commitExitReview(review_, decisions_));
                const auto reopened = editor_->requestOpen({owner.summary().key, "after-cancel"});
                assert(reopened && editor_->acknowledgeOpen(*reopened));
                assert(owner.redo());
                review_ = *editor_->beginExitReview();
                saved_ = owner.historyView()->history.current;
                save_ = *owner.requestSave("review-S1");
                assert(owner.undo());
                ++review_stage_;
                std::puts(
                    "exit-review: late dirty document included; old version rejected; cancellation restores admission");
                return;
            }

            const auto status = owner.saveStatus(save_);
            assert(status);
            if (std::holds_alternative<SavePending>(*status))
            {
                return;
            }
            assert(std::holds_alternative<SaveSucceeded>(*status));
            assert(std::get<SaveSucceeded>(*status).captured == saved_ && !owner.historyView()->history.clean);
            assert(owner.acknowledgeSave(save_));
            decisions_ = decisions();
            auto &choice = *std::ranges::find(decisions_, handle_, &DocumentCloseDecision::document);
            choice.decision = EDocumentCloseDecision::CLOSE_CLEAN;
            const auto refused = editor_->commitExitReview(review_, decisions_);
            assert(!refused && refused.error().code == EEditorError::INVALID_STATE);
            assert(evidence_.view_close_calls == 0);
            choice.decision = EDocumentCloseDecision::DISCARD_THIS_STATE;
            assert(editor_->commitExitReview(review_, decisions_));
            assert(evidence_.view_close_calls == 0 && evidence_.view_destructors == 0);
            for (const auto &summary : editor_->documents())
            {
                assert(editor_->document(summary.handle)->get().closeStatus().state == ECloseState::CLOSING);
            }
            evidence_.correct = true;
            evidence_.checked = true;
            std::puts(
                "exit-review: saved S1 cannot authorize dirty S2; exact decisions latch all owners without callbacks");
        }

        material::MaterialEditor &document()
        {
            return dynamic_cast<material::MaterialEditor &>(editor_->document(handle_)->get());
        }
        void finish()
        {
            evidence_.checked = true;
            exit_.request(*editor_);
        }
        Evidence &evidence_;
        lux::object::ObjectMessageQueue observer_queue_;
        NoticeReceiver receiver_{observer_queue_.dispatcherRef(), evidence_};
        Editor *editor_{};
        OpenRequestId open_;
        DocumentHandle handle_;
        material::MaterialCompileId compile_;
        lux::object::ScopedConnection connection_;
        lux::object::ScopedConnection second_connection_;
        lux::object::ScopedConnection queued_connection_;
        bool release_late_{};
        OpenRequestId late_;
        ExitReviewId review_;
        std::vector<DocumentCloseDecision> decisions_;
        SaveRequestId save_;
        editing::StateId saved_;
        unsigned review_stage_{};
        std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
    };
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 3);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    lux::meta::ReflectionRegistry::initRegistry();
    const auto root =
        std::filesystem::path(argv[2]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    createProject(root);
    Evidence evidence{argv[1]};
    EditorConfig config;
    config.project_file = root / "Project.luxproject";
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.frontend = [&evidence] { return std::make_unique<Frontend>(evidence); };
    Editor editor(std::move(config));
    const int result = editor.exec();
    std::printf("review-result: mode=%s exec=%d checked=%u correct=%u closed=%u view_destructors=%u\n", argv[1], result,
                evidence.checked, evidence.correct, evidence.frontend_closed, evidence.view_destructors);
    return result == 0 && evidence.checked && evidence.correct && evidence.frontend_closed &&
                   evidence.view_destructors == 1
               ? 0
               : 10;
}
