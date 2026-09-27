#include <lux/engine/editor/ui/TaskPane.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <map>
#include <tuple>

namespace lux::editor::ui
{
    struct TaskPane::Impl final
    {
        using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;
        static Key key(process::TaskId id) noexcept
        {
            return {id.runtime, id.slot.index, id.slot.gen};
        }

        class Content final : public lux::ui::Element
        {
        public:
            Content(TaskPane& parent, Impl& owner) : Element(parent, lux::ui::ElementId{"tasks"}), owner_(owner) {}

        private:
            void draw() noexcept override
            {
                owner_.draw();
            }
            Impl& owner_;
        };

        TaskPane& pane_;
        EditorContext& context_;
        std::vector<process::TaskInfo> rows_;
        std::map<Key, std::size_t> indices_;
        std::vector<process::TaskId> changed_;
        std::vector<process::TaskId> cancel_;
        std::uint64_t revision_{};
        bool reset_{true};
        bool hide_{};
        Content content_;
        object::Connection changed_connection_, reset_connection_, close_connection_;

        Impl(TaskPane& pane, EditorContext& context, EditorResult<void>& status)
            : pane_(pane), context_(context), content_(pane, *this)
        {
            pane_.setContent(content_);
            changed_connection_ = detail::takeConnection(
                object::LuxObject::connect(
                    &context_,
                    &EditorContext::taskChanged,
                    [this](process::TaskId id) noexcept { changed_.push_back(id); }
                ),
                status
            );
            reset_connection_ = detail::takeConnection(
                object::LuxObject::connect(&context_, &EditorContext::tasksReset, [this]() noexcept { reset_ = true; }),
                status
            );
            close_connection_ = detail::takeConnection(
                object::LuxObject::connect(&pane_, &lux::ui::Pane::closeRequested, [this]() noexcept { hide_ = true; }),
                status
            );
            update();
        }

        void update() noexcept
        {
            if (std::exchange(hide_, false))
                pane_.setVisible(false);
            auto& runtime = context_.execution();
            for (auto id : cancel_)
                static_cast<void>(runtime.requestStop(id));
            cancel_.clear();
            const auto revision = context_.taskRevision();
            const bool missed_notification = revision - revision_ != changed_.size();
            if (std::exchange(reset_, false) || missed_notification)
            {
                rows_ = runtime.taskInfos();
                indices_.clear();
                for (std::size_t i{}; i < rows_.size(); ++i)
                    indices_.emplace(key(rows_[i].id), i);
            }
            else
                for (auto id : changed_)
                {
                    auto value = runtime.taskInfo(id);
                    auto found = indices_.find(key(id));
                    if (value)
                    {
                        if (found != indices_.end())
                            rows_[found->second] = std::move(*value);
                        else
                        {
                            indices_.emplace(key(id), rows_.size());
                            rows_.push_back(std::move(*value));
                        }
                    }
                    else if (found != indices_.end())
                    {
                        const auto index = found->second;
                        indices_.erase(found);
                        if (index + 1 != rows_.size())
                        {
                            rows_[index] = std::move(rows_.back());
                            indices_[key(rows_[index].id)] = index;
                        }
                        rows_.pop_back();
                    }
                }
            changed_.clear();
            revision_ = revision;
        }

        void draw() noexcept
        {
            constexpr const char* states[]{"Queued", "Running", "Succeeded", "Failed", "Cancelled"};
            if (!ImGui::BeginTable("tasks", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
                return;
            ImGui::TableSetupColumn("Task");
            ImGui::TableSetupColumn("Stage");
            ImGui::TableSetupColumn("Progress");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows_.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& task = rows_[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(task.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(task.phase.c_str());
                    ImGui::TableNextColumn();
                    if (task.progress && task.progress->total != 0)
                        ImGui::Text(
                            "%.0f%%",
                            100.0 * static_cast<double>(task.progress->completed) /
                                static_cast<double>(task.progress->total)
                        );
                    else
                        ImGui::TextUnformatted("--");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(states[static_cast<unsigned>(task.state)]);
                    ImGui::TableNextColumn();
                    if (!task.finished)
                    {
                        ImGui::BeginDisabled(task.cancel_requested);
                        if (ImGui::SmallButton("Cancel"))
                            cancel_.push_back(task.id);
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    };

    TaskPane::TaskPane(lux::ui::Root& root, EditorContext& context, EditorResult<void>& status)
        : Pane(root, lux::ui::PaneId{"tasks"}, lux::ui::PaneTypeId{"lux.editor.tasks"}, "Background tasks"),
          impl_(std::make_unique<Impl>(*this, context, status))
    {}
    TaskPane::~TaskPane() noexcept = default;
    void TaskPane::update() noexcept
    {
        impl_->update();
    }
}
