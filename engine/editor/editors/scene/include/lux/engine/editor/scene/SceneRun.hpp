#pragma once

#include <chrono>
#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>

namespace lux::editor::rendering
{
    class RenderView;
    struct ViewConfig;
} // namespace lux::editor::rendering

namespace lux::editor::scene
{
    struct RunId final
    {
        DocumentHandle document;
        std::uint64_t serial{};
        friend bool operator==(RunId, RunId) = default;
    };

    enum class ERunState : std::uint8_t
    {
        IDLE,
        PREPARING,
        RUNNING,
        PAUSED,
        STOPPING,
        FINISHED,
        FAILED
    };

    struct RunStatus final
    {
        RunId id;
        ERunState state{ERunState::IDLE};
        bool pause_pending{};
        editing::StateId captured_state;
        std::uint64_t steps{};
        std::chrono::nanoseconds elapsed{};
        lux::render::RenderSceneId render_scene;
        std::uint64_t published_updates{}, forwarded_updates{}, backpressure_count{};
        std::uint32_t pending_updates{}, update_high_water{};
        std::size_t retained_resources{};
        std::chrono::nanoseconds simulation_work{}, publication_wait{};
        EditorResult<void> result;
    };

    namespace detail
    {
        struct RunViewCount final
        {
            std::size_t value{};
        };
        class SceneRun;
        struct SceneRunSlot final
        {
            editing::HistoryId owner;
        };
    } // namespace detail

    // The Pane owns the RenderView. This lease prevents Run scene/resource release
    // until the Pane completes the existing CPU-reference and GPU-watermark close.
    class LUX_EDITOR_SCENE_PUBLIC RunViewLease final
    {
      public:
        ~RunViewLease();
        RunViewLease(RunViewLease &&) noexcept;
        RunViewLease &operator=(RunViewLease &&) noexcept;
        RunViewLease(const RunViewLease &) = delete;
        RunViewLease &operator=(const RunViewLease &) = delete;
        [[nodiscard]] rendering::RenderView &view() noexcept
        {
            return *view_;
        }

      private:
        friend class detail::SceneRun;
        RunViewLease(std::unique_ptr<rendering::RenderView>, std::shared_ptr<detail::RunViewCount>);
        void reset() noexcept;
        std::shared_ptr<detail::RunViewCount> count_;
        std::unique_ptr<rendering::RenderView> view_;
    };
} // namespace lux::editor::scene
