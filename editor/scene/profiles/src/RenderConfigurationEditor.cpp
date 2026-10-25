#include <algorithm>
#include <imgui.h>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/RenderConfigurationEditor.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor
{
    namespace
    {
        class RenderConfigurationEditor final : public ui::Element
        {
        public:
            RenderConfigurationEditor(EditorContext& context, std::span<const std::byte> bytes) noexcept
            {
                valid_ = scene::renderConfigurationCodec().decode(bytes, &configuration_).has_value();
                auto rendering = context.extensions().find<rendering::RenderExecution>();
                if (!rendering)
                {
                    status_ = "Render is unavailable in this session; the stored configuration is preserved";
                    return;
                }
                auto& client = rendering->mainClient();
                auto command = client.bind<render::ListCapabilities>();
                if (!command)
                {
                    status_ = "Capability discovery is unavailable";
                    return;
                }
                const auto target = objectId();
                auto work = execution::taskResult(
                    stdexec::starts_on(context.engine().execution().mainScheduler(), query(client, *command))
                );
                auto accepted = context.engine().execution().spawn(stdexec::then(
                    std::move(work),
                    [target](auto&& result) noexcept
                    {
                        auto found = object::ObjectRuntime::instance().resolve(target);
                        if (!found)
                        {
                            return;
                        }
                        auto& editor = *static_cast<RenderConfigurationEditor*>(*found);
                        if (result)
                        {
                            editor.supports_ = std::move(*result);
                            editor.status_ = "Select a supported capability and implementation";
                        }
                        else
                        {
                            editor.status_ = "Capability discovery failed; the stored configuration is preserved";
                        }
                        if (auto* pane = editor.containingPane())
                        {
                            pane->requestUpdate();
                        }
                    }
                ));
                if (!accepted)
                {
                    status_ = "Capability discovery could not be scheduled";
                }
            }

            bool serialize(std::vector<std::byte>& output) const noexcept
            {
                return valid_ && scene::renderConfigurationCodec().encode(&configuration_, output).has_value();
            }

        private:
            static exec::task<render::RenderResult<std::vector<render::CapabilitySupport>>> query(
                rendering::RenderRequester& client,
                rendering::BoundCommand<render::ListCapabilities> command
            ) noexcept
            {
                // The coroutine owns its request; no pane/Context borrow crosses the response wait.
                render::CapabilityQuery input;
                auto result = co_await client.request(command, input);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                co_return std::move(result.implementations);
            }

            static const char* unavailable(
                const render::CapabilitySupport& support,
                const render::RenderCapability& capability
            ) noexcept
            {
                if (support.unavailable.type != 0)
                {
                    return "Unavailable on this device";
                }
                if (!support.portable_configuration)
                {
                    return "This implementation has no portable configuration codec";
                }
                if (capability.outputs.empty())
                {
                    return "Resource capability; it does not produce a Scene output";
                }
                for (const auto& input : capability.inputs)
                {
                    if (input.required && !scene::supportsSceneRenderInput(input))
                    {
                        return "A required input producer is not installed in the Scene system";
                    }
                }
                return nullptr;
            }

            void drawConfiguration() noexcept
            {
                const auto& encoded = configuration_.configuration;
                if (encoded.schema != render::kLitSceneConfigurationSchema || encoded.version != 1)
                {
                    ImGui::TextWrapped("No field editor installed; existing configuration bytes are preserved.");
                    return;
                }
                auto value = render::decodeLitSceneConfiguration(encoded.bytes);
                if (!value)
                {
                    ImGui::TextWrapped("Stored configuration is invalid; choose a supported default to replace it.");
                    return;
                }
                bool edited = ImGui::Checkbox("Shadows", &value->shadows);
                edited |= ImGui::InputScalar("Shadow resolution", ImGuiDataType_U32, &value->shadow_resolution);
                edited |= ImGui::InputScalar("Shadow filter radius", ImGuiDataType_U32, &value->shadow_filter_radius);
                edited |= ImGui::DragFloat("Shadow distance", &value->shadow_distance, 1.F, 0.1F, 1.0e6F);
                edited |=
                    ImGui::InputScalar("Cluster index capacity", ImGuiDataType_U32, &value->cluster_index_capacity);
                if (edited)
                {
                    auto replacement = render::encodeLitSceneConfiguration(*value);
                    if (replacement)
                    {
                        configuration_.configuration = std::move(*replacement);
                        pane().requestUpdate();
                    }
                    else
                    {
                        status_ = "Invalid configuration; the previous value is preserved";
                    }
                }
            }

            ui::SizeHint sizeHintContent() noexcept override
            {
                ui::SizeHint hint;
                hint.minimum = {180, 150};
                hint.preferred = {360, 230};
                return hint;
            }

            void draw() noexcept override
            {
                ImGui::BeginChild("render-capability", {rect().size.width, rect().size.height});
                ImGui::TextWrapped("%s", status_.c_str());
                const auto preview = configuration_.capability + " / " + configuration_.implementation;
                if (ImGui::BeginCombo("Render capability", preview.c_str()))
                {
                    for (const auto& support : supports_)
                    {
                        for (const auto& capability : support.capabilities)
                        {
                            const auto label = capability.name + " / " + support.implementation;
                            const auto* reason = unavailable(support, capability);
                            ImGui::BeginDisabled(reason != nullptr);
                            const bool selected = configuration_.capability == capability.name &&
                                                  configuration_.implementation == support.implementation;
                            if (ImGui::Selectable(label.c_str(), selected) && !selected)
                            {
                                configuration_ = {};
                                configuration_.capability = capability.name;
                                configuration_.capability_version = capability.version;
                                configuration_.implementation = support.implementation;
                                configuration_.implementation_version = support.implementation_version;
                                configuration_.configuration = {
                                    support.configuration.schema,
                                    support.configuration.version,
                                    support.default_configuration
                                };
                                valid_ = true;
                                pane().requestUpdate();
                            }
                            ImGui::EndDisabled();
                            if (reason)
                            {
                                ImGui::TextDisabled("%s", reason);
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                for (const auto& support : supports_)
                {
                    if (support.implementation != configuration_.implementation)
                    {
                        continue;
                    }
                    ImGui::TextWrapped("%s", support.description.c_str());
                    ImGui::Text(
                        "Configuration: %s v%u (%zu bytes)",
                        configuration_.configuration.schema.c_str(),
                        configuration_.configuration.version,
                        configuration_.configuration.bytes.size()
                    );
                    drawConfiguration();
                    for (const auto& capability : support.capabilities)
                    {
                        if (capability.name != configuration_.capability)
                        {
                            continue;
                        }
                        for (const auto& input : capability.inputs)
                        {
                            ImGui::TextWrapped(
                                "%s v%u (%s): %s",
                                input.schema.c_str(),
                                input.version,
                                input.required ? "required" : "optional",
                                input.description.c_str()
                            );
                        }
                    }
                }
                ImGui::EndChild();
            }

            scene::RenderSystemConfiguration configuration_;
            std::vector<render::CapabilitySupport> supports_;
            std::string status_ = "Reading Server capabilities...";
            bool valid_ = false;
        };
    } // namespace

    FrameworkResult<void> registerRenderConfigurationEditor(EditorComposition& composition) noexcept
    {
        return composition.registerSceneConfigurationEditor(
            {{},
             cxx::typeToken<scene::RenderSystemConfiguration>(),
             +[](EditorContext& context, std::span<const std::byte> bytes) noexcept -> std::unique_ptr<ui::Element>
             { return std::make_unique<RenderConfigurationEditor>(context, bytes); },
             +[](const ui::Element& element, std::vector<std::byte>& output) noexcept
             { return static_cast<const RenderConfigurationEditor&>(element).serialize(output); }}
        );
    }
} // namespace lux::editor
