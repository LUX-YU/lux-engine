#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    void EditorApplication::Impl::receiveArtifact(persistence::DerivedArtifact source)
    {
        const bool invalid = !source.valid();
        if (invalid)
        {
            result_failure_ = EditorFailure{EEditorError::INVALID_ARGUMENT, "artifact.input"};
            return;
        }
        if (phase_ != EApplicationPhase::RUNNING || artifacts_.size() == 64 || next_artifact_ == UINT64_MAX)
        {
            result_failure_ = EditorFailure{
                EEditorError::BUSY,
                "artifact.admission",
                0,
                "Close is pending or publication result capacity is full."
            };
            return;
        }
        // Called by a DIRECT intent during UI maintenance: retain the immutable capture only.
        artifacts_.push_back({next_artifact_++, std::move(source)});
    }
    EditorResult<void> EditorApplication::Impl::settleArtifacts()
    {
        for (auto& entry : artifacts_)
        {
            if (entry.terminal())
                continue;
            if (entry.pending)
            {
                if (phase_ != EApplicationPhase::RUNNING)
                {
                    entry.failure = EditorFailure{EEditorError::CLOSING, "artifact.admission"};
                    continue;
                }
                auto accepted = ArtifactPublicationOperation::create(*entry.pending, sessions_, *project_,
                    engine_->execution(), writes_, files_, save_execution_);
                if (!accepted)
                {
                    if (accepted.error().code != EEditorError::BUSY)
                        entry.failure = accepted.error();
                    continue;
                }
                entry.operation = std::move(*accepted);
                entry.pending.reset();
            }
            entry.operation->update();
        }
        return {};
    }
}
