#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/editor/extensions/builtin/physics2d_visibility.h>
#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <array>
#include <exception>
#include <new>

extern "C" void lux_physics2d_configuration_meta(lux::meta::ReflectionRegistry&, lux::meta::qual_type_index_fix_list&);

namespace
{
    class PhysicsConfigurationElement final : public lux::ui::Element
    {
    public:
        PhysicsConfigurationElement(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            lux::editor::ConfigurationValue& value,
            lux::editor::EditorResult<void>& status
        )
            : Element(parent, std::move(id)), value_(value),
              layout_(*this, lux::ui::ElementId{"fields"}, lux::ui::ELayoutType::FORM),
              x_label_(layout_, lux::ui::ElementId{"x-label"}, "Gravity X"),
              x_(layout_, lux::ui::ElementId{"x"}, config().gravity_x),
              y_label_(layout_, lux::ui::ElementId{"y-label"}, "Gravity Y"),
              y_(layout_, lux::ui::ElementId{"y"}, config().gravity_y),
              step_label_(layout_, lux::ui::ElementId{"step-label"}, "Fixed step (ns)"),
              step_(layout_, lux::ui::ElementId{"step"}, config().fixed_step_nanoseconds),
              substeps_label_(layout_, lux::ui::ElementId{"substeps-label"}, "Maximum substeps"),
              substeps_(layout_, lux::ui::ElementId{"substeps"}, config().max_substeps),
              capacity_label_(layout_, lux::ui::ElementId{"capacity-label"}, "Body capacity"),
              capacity_(layout_, lux::ui::ElementId{"capacity"}, config().body_capacity)
        {
            const std::array fields{&x_, &y_, &step_, &substeps_, &capacity_};
            for (std::size_t i{}; i < fields.size(); ++i)
            {
                auto connection = lux::object::LuxObject::connect(
                    fields[i],
                    &lux::ui::NumericEdit::edited,
                    [this](lux::ui::EditResult result) noexcept
                    {
                        if (!result.changed && !result.cancelled)
                            return;
                        auto& value = config();
                        value.gravity_x = std::get<double>(x_.value());
                        value.gravity_y = std::get<double>(y_.value());
                        value.fixed_step_nanoseconds = std::get<std::int64_t>(step_.value());
                        value.max_substeps = std::get<std::uint32_t>(substeps_.value());
                        value.body_capacity = std::get<std::uint32_t>(capacity_.value());
                    }
                );
                if (connection)
                    connections_[i] = std::move(*connection);
                else
                {
                    if (connection.error() == lux::object::EConnectError::ALLOCATION_FAILURE)
                        std::terminate();
                    const bool is_capacity_exhausted =
                        connection.error() == lux::object::EConnectError::CAPACITY_EXHAUSTED;
                    status = lux::cxx::unexpected(lux::editor::EditorFailure{
                        is_capacity_exhausted ? lux::editor::EEditorError::CAPACITY
                                              : lux::editor::EEditorError::FRONTEND_FAILURE,
                        "physics.configuration.connect"
                    });
                }
            }
        }

    private:
        lux::physics2d::Physics2DSystemConfiguration& config() noexcept
        {
            return *static_cast<lux::physics2d::Physics2DSystemConfiguration*>(value_.data());
        }
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        void update() noexcept override
        {
            const auto& value = config();
            if (!x_.editing())
                x_.setValue(value.gravity_x);
            if (!y_.editing())
                y_.setValue(value.gravity_y);
            if (!step_.editing())
                step_.setValue(value.fixed_step_nanoseconds);
            if (!substeps_.editing())
                substeps_.setValue(value.max_substeps);
            if (!capacity_.editing())
                capacity_.setValue(value.body_capacity);
        }
        lux::editor::ConfigurationValue& value_;
        lux::ui::Layout layout_;
        lux::ui::Label x_label_;
        lux::ui::NumericEdit x_;
        lux::ui::Label y_label_;
        lux::ui::NumericEdit y_;
        lux::ui::Label step_label_;
        lux::ui::NumericEdit step_;
        lux::ui::Label substeps_label_;
        lux::ui::NumericEdit substeps_;
        lux::ui::Label capacity_label_;
        lux::ui::NumericEdit capacity_;
        std::array<lux::object::Connection, 5> connections_;
    };
    lux::editor::EditorResult<std::unique_ptr<lux::ui::Element>> createConfiguration(
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        lux::editor::ConfigurationValue& value
    ) noexcept
    try
    {
        lux::editor::EditorResult<void> status;
        auto result = std::make_unique<PhysicsConfigurationElement>(parent, std::move(id), value, status);
        if (!status)
            return lux::cxx::unexpected(status.error());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(
            lux::editor::EditorFailure{lux::editor::EEditorError::FRONTEND_FAILURE, "physics.configuration.create"}
        );
    }
} // namespace

extern "C" LUX_PHYSICS2D_EDITOR_PUBLIC const lux::editor::extensions::EditorExtensionExports* lux_editor_exports_v10(
) noexcept
{
    using namespace lux::editor;
    static const extensions::EditorExtensionExports exports{
        .counts = {.reflection = 1, .services = 1},
        .contribute =
            +[](extensions::ContributionDraft& draft, lux::object::CodeLease code) -> extensions::ContributionResult<void>
        {
            draft.reflection.push_back({code, &lux_physics2d_configuration_meta, &scene::validateSceneEditors});
            const auto& system = lux::physics2d::physics2DSystemRegistrations().front();
            scene::SceneEditorCatalog::Definition definition;
            definition.configurations.push_back(
                {code,
                 {"lux.physics2d.Configuration",
                  1,
                  system.configuration,
                  +[](lux::meta::ReflectionRegistry& registry) noexcept {
                      return registry.findClass(
                          lux::cxx::typeToken<lux::physics2d::Physics2DSystemConfiguration>().name()
                      );
                  }},
                 &createConfiguration}
            );
            draft.services.push_back(scene::declareSceneEditors(
                code, lux::services::ServiceNameView{"lux.editor.physics2d.configuration"}, std::move(definition)
            ));
            return {};
        }
    };
    return &exports;
}
