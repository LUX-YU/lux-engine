#pragma once
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/ui/Element.hpp>
namespace lux::editor
{
    class PluginSettingsElement final : public lux::ui::Element
    {
    public:
        PluginSettingsElement(lux::ui::Element&, EditorContext&);

    private:
        void draw() noexcept override;
        void update() noexcept override;
        ProjectStorage& project_;
        const lux::project::PluginManager& plugins_;
        process::ExecutionRuntime& execution_;
        std::vector<ProjectPluginEntry> selection_;
        bool save_requested_{}, retry_requested_{}, abandon_requested_{};
        std::string status_;
    };
}
