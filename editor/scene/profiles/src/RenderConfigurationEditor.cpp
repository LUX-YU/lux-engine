#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <algorithm>
#include <imgui.h>
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/BuiltinValueTypes.value_editor.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/RenderConfigurationEditor.hpp>
#include <lux/engine/editor/SceneConfigurationForm.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/systems/render/RenderExecution.hpp>
#include <lux/engine/systems/render/RenderSystemConfiguration.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor
{
    namespace
    {
        template <class Value, auto Decode, auto Encode> class ConfigurationFields final : public SceneConfigurationForm
        {
        public:
            explicit ConfigurationFields(std::span<const std::byte> bytes) noexcept
            {
                if (bytes.empty())
                {
                    valid_ = true;
                    return;
                }
                auto value = Decode(bytes);
                valid_ = bool(value);
                if (value)
                {
                    value_ = std::move(*value);
                }
            }

            bool serialize(std::vector<std::byte>& output) const noexcept override
            {
                if (!valid_)
                {
                    return false;
                }
                auto result = Encode(value_);
                if (!result)
                {
                    return false;
                }
                output = std::move(result->bytes);
                return true;
            }

        private:
            ui::SizeHint sizeHintContent() noexcept override
            {
                ui::SizeHint result;
                result.minimum = {180, 32};
                result.preferred = {320, 160};
                return result;
            }

            void draw() noexcept override
            {
                if (!valid_)
                {
                    ImGui::TextWrapped("Invalid configuration bytes; reset the selected contribution to edit.");
                    return;
                }
                if (editValue(ValueEditOptions{}, value_))
                {
                    invalid_fields_ = !Encode(value_);
                    static_cast<void>(emit(edited));
                    requestUpdate();
                }
                if (invalid_fields_)
                {
                    ImGui::TextWrapped("Invalid field value; Apply will preserve the current configuration.");
                }
            }

            Value value_{};
            bool valid_ = false;
            bool invalid_fields_ = false;
        };

        template <class Value, auto Decode, auto Encode>
        FrameworkResult<void> registerConfigurationFields(EditorComposition& composition) noexcept
        {
            using Widget = ConfigurationFields<Value, Decode, Encode>;
            return composition.registerSceneConfigurationEditor(
                {{},
                 cxx::make_basic_type_info<Value>(),
                 +[](EditorContext&, std::span<const std::byte> bytes
                  ) noexcept -> std::unique_ptr<SceneConfigurationForm> { return std::make_unique<Widget>(bytes); }}
            );
        }

        class RenderConfigurationEditor final : public SceneConfigurationForm
        {
        public:
            RenderConfigurationEditor(EditorContext& context, std::span<const std::byte> bytes) noexcept
                : context_(context)
            {
                valid_ = ::lux::engine::render::renderConfigurationCodec().decode(bytes, &configuration_).has_value();
                auto* rendering = context.extensions().find<::lux::engine::render::RenderExecution>();
                if (!rendering)
                {
                    status_ = "Render declarations are unavailable; stored bytes are preserved";
                    return;
                }
                auto& client = rendering->mainClient();
                auto command = client.bind<lux::render::ListCapabilities>();
                if (!command)
                {
                    status_ = "Capability discovery unavailable";
                    return;
                }
                const auto target = objectId();
                auto work = execution::asResult(
                    stdexec::starts_on(execution::ExecutionRuntime::instance().mainScheduler(), query(client, *command))
                );
                auto accepted = execution::ExecutionRuntime::instance().spawn(stdexec::then(
                    std::move(work),
                    [target](auto&& result) noexcept
                    {
                        auto resolved = object::ObjectRuntime::instance().resolve(target);
                        if (!resolved)
                        {
                            return;
                        }
                        auto& editor = *static_cast<RenderConfigurationEditor*>(*resolved);
                        if (result)
                        {
                            editor.supports_ = std::move(*result);
                            editor.rebuildDrafts();
                            editor.status_ = "Select capabilities, their implementation settings, and a final output";
                        }
                        else
                        {
                            editor.status_ = "Capability discovery failed; configuration is preserved";
                        }
                        editor.requestUpdate();
                    }
                ));
                if (!accepted)
                {
                    status_ = "Capability discovery could not be scheduled";
                }
            }

            ~RenderConfigurationEditor() noexcept override
            {
                clearElements();
            }

            bool serialize(std::vector<std::byte>& output) const noexcept override
            {
                auto candidate = pending_configuration_ ? *pending_configuration_ : configuration_;
                if (!valid_ || !drafts_valid_ || (!pending_configuration_ && !captureDrafts(candidate)))
                {
                    return false;
                }
                // Port/provider/device qualification belongs to the shared creation candidate, not this widget.
                return ::lux::engine::render::renderConfigurationCodec().encode(&candidate, output).has_value();
            }

        private:
            struct Draft final
            {
                std::unique_ptr<SceneConfigurationForm> widget;
            };

            static exec::task<lux::render::RenderResult<std::vector<lux::render::CapabilityImplementationInfo>>> query(
                ::lux::engine::render::RenderRequester& client,
                ::lux::engine::render::BoundCommand<lux::render::ListCapabilities> command
            ) noexcept
            {
                lux::render::CapabilityQuery input;
                auto result = co_await client.request(command, input);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                co_return std::move(result.implementations);
            }

            const lux::render::CapabilityImplementationInfo*
            support(const lux::render::CapabilityContributionDescription& contribution) const noexcept
            {
                const auto found = std::ranges::find_if(
                    supports_,
                    [&](const auto& value)
                    {
                        return value.implementation == contribution.implementation &&
                               value.capability.type.name() == contribution.capability;
                    }
                );
                return found == supports_.end() ? nullptr : &*found;
            }

            bool captureDrafts(
                ::lux::engine::render::RenderSystemConfiguration& candidate,
                std::optional<std::size_t> excluded = {}
            ) const noexcept
            {
                for (std::size_t i = 0; i < drafts_.size(); ++i)
                {
                    if (excluded != i && drafts_[i].widget &&
                        !drafts_[i].widget->serialize(candidate.composition.contributions[i].configuration.bytes))
                    {
                        return false;
                    }
                }
                return true;
            }

            ::lux::engine::render::RenderSystemConfiguration*
            beginChange(std::optional<std::size_t> excluded = {}) noexcept
            {
                if (!pending_configuration_)
                {
                    auto candidate = configuration_;
                    if (!captureDrafts(candidate, excluded))
                    {
                        status_ = "Correct invalid fields before changing contributions";
                        return nullptr;
                    }
                    pending_configuration_ = std::move(candidate);
                }
                requestUpdate();
                return &*pending_configuration_;
            }

            void update() noexcept override
            {
                if (pending_configuration_)
                {
                    configuration_ = std::move(*pending_configuration_);
                    pending_configuration_.reset();
                    rebuildDrafts();
                    static_cast<void>(emit(edited));
                }
            }

            void rebuildDrafts() noexcept
            {
                // Recompute only when discovery or the selected composition changes.
                // draw() consumes these flags; it does not resolve dependency metadata.
                available_.clear();
                available_.reserve(supports_.size());
                for (const auto& value : supports_)
                {
                    const bool prerequisites_present = std::ranges::all_of(
                        value.capability.prerequisites,
                        [&](auto prerequisite) noexcept
                        {
                            return std::ranges::any_of(
                                configuration_.composition.contributions,
                                [&](const auto& item) noexcept
                                {
                                    const auto* selected = support(item);
                                    return selected && selected->capability.type == prerequisite;
                                }
                            );
                        }
                    );
                    available_.push_back(
                        value.unavailable.type == 0 && value.portable_configuration && prerequisites_present
                    );
                }
                draft_connections_.clear();
                clearElements();
                drafts_.clear();
                drafts_valid_ = true;
                for (const auto& contribution : configuration_.composition.contributions)
                {
                    auto& draft = drafts_.emplace_back();
                    const auto* value = support(contribution);
                    if (!value)
                    {
                        continue;
                    }
                    const auto& encoded = contribution.configuration;
                    if (encoded.schema != value->configuration.schema ||
                        encoded.version != value->configuration.version)
                    {
                        continue;
                    }
                    const auto* editor = context_.sceneConfigurationEditor(value->config_type);
                    if (editor)
                    {
                        draft.widget = editor->create(context_, encoded.bytes);
                        if (!draft.widget || !addElement(*draft.widget))
                        {
                            drafts_valid_ = false;
                            status_ = "The selected configuration editor could not be created or attached";
                            continue;
                        }
                        auto connected = connect(
                            draft.widget.get(),
                            &SceneConfigurationForm::edited,
                            this,
                            [this]() noexcept { static_cast<void>(emit(edited)); }
                        );
                        if (!connected)
                        {
                            drafts_valid_ = false;
                            status_ = "The selected configuration editor could not connect its change signal";
                            continue;
                        }
                        draft_connections_.push_back(std::move(*connected));
                    }
                }
            }

            bool acceptsElements() const noexcept override
            {
                return true;
            }

            ui::SizeHint sizeHintContent() noexcept override
            {
                ui::SizeHint hint;
                hint.minimum = {220, 180};
                hint.preferred = {460, 440};
                return hint;
            }

            void draw() noexcept override
            {
                ImGui::BeginChild("render-composition", {rect().size.width, rect().size.height});
                ImGui::TextWrapped("%s", status_.c_str());
                auto& composition = configuration_.composition;
                std::optional<std::size_t> remove;
                for (std::size_t i = 0; i < composition.contributions.size(); ++i)
                {
                    auto& contribution = composition.contributions[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::SeparatorText(contribution.key.c_str());
                    ImGui::TextWrapped("%s / %s", contribution.capability.c_str(), contribution.implementation.c_str());
                    if (ImGui::SmallButton("Remove"))
                    {
                        remove = i;
                    }
                    const auto* value = support(contribution);
                    if (value)
                    {
                        if (ImGui::SmallButton("Reset configuration"))
                        {
                            if (auto* candidate = beginChange(i))
                            {
                                candidate->composition.contributions[i]
                                    .configuration = {value->configuration.schema, value->configuration.version, {}};
                            }
                        }
                    }
                    if (i < drafts_.size() && drafts_[i].widget)
                    {
                        auto& child = *drafts_[i].widget;
                        const auto origin = ImGui::GetCursorScreenPos();
                        const auto size = child.measure(ImGui::GetContentRegionAvail().x).preferred;
                        child.arrange({{}, size});
                        drawChild(child, {origin.x - contentOrigin().x, origin.y - contentOrigin().y});
                        ImGui::SetCursorScreenPos(origin);
                        ImGui::Dummy({size.width, size.height});
                    }
                    else
                    {
                        ImGui::TextWrapped("No matching field editor; existing bytes are preserved.");
                    }
                    ImGui::PopID();
                }
                if (remove)
                {
                    auto* candidate = beginChange();
                    if (candidate)
                    {
                        const auto key = composition.contributions[*remove].key;
                        auto& edited = candidate->composition;
                        std::erase_if(edited.outputs, [&](const auto& output) { return output.contribution == key; });
                        std::erase_if(
                            edited.contributions,
                            [&](const auto& contribution) { return contribution.key == key; }
                        );
                    }
                }
                if (ImGui::BeginCombo("Add contribution", "Select a capability"))
                {
                    for (std::size_t option = 0; option < supports_.size(); ++option)
                    {
                        const auto& value = supports_[option];
                        const auto& capability = value.capability;
                        if (value.outputs.empty())
                        {
                            continue;
                        }
                        const auto label = std::string(capability.type.name()) + " / " + value.implementation;
                        ImGui::BeginDisabled(!available_[option]);
                        if (ImGui::Selectable(label.c_str()))
                        {
                            auto* candidate = beginChange();
                            if (candidate)
                            {
                                auto& edited = candidate->composition;
                                std::string key;
                                do
                                {
                                    key = "contribution" + std::to_string(++next_key_);
                                } while (std::ranges::any_of(
                                    edited.contributions,
                                    [&](const auto& item) { return item.key == key; }
                                ));
                                edited.contributions.push_back(
                                    {std::move(key),
                                     std::string(capability.type.name()),
                                     value.implementation,
                                     {value.configuration.schema, value.configuration.version, {}}}
                                );
                                valid_ = true;
                            }
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                const auto final = composition.outputs.empty() ? std::string("Select final output")
                                                               : composition.outputs.front().contribution + "/" +
                                                                     composition.outputs.front().name;
                if (ImGui::BeginCombo("Final output", final.c_str()))
                {
                    for (const auto& contribution : composition.contributions)
                    {
                        const auto* selected = support(contribution);
                        if (!selected)
                        {
                            continue;
                        }
                        for (const auto& output : selected->outputs)
                        {
                            const auto label = contribution.key + "/" + output;
                            if (ImGui::Selectable(label.c_str(), label == final))
                            {
                                if (auto* candidate = beginChange())
                                {
                                    candidate->composition.outputs = {{contribution.key, output}};
                                }
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndChild();
            }

            EditorContext& context_;
            ::lux::engine::render::RenderSystemConfiguration configuration_;
            std::optional<::lux::engine::render::RenderSystemConfiguration> pending_configuration_;
            std::vector<lux::render::CapabilityImplementationInfo> supports_;
            std::vector<Draft> drafts_;
            std::vector<bool> available_;
            std::vector<object::Connection> draft_connections_;
            std::string status_ = "Reading Render capabilities...";
            std::uint64_t next_key_ = 0;
            bool valid_ = false;
            bool drafts_valid_ = true;
        };
    } // namespace

    FrameworkResult<void> registerRenderConfigurationEditor(EditorComposition& composition) noexcept
    {
        using namespace lux::render;
        auto result = registerConfigurationFields<
            RasterSurfaceConfiguration,
            decodeRasterSurfaceConfiguration,
            encodeRasterSurfaceConfiguration>(composition);
        if (result)
        {
            result = registerConfigurationFields<
                DeferredLightingConfiguration,
                decodeDeferredLightingConfiguration,
                encodeDeferredLightingConfiguration>(composition);
        }
        if (!result)
        {
            return result;
        }
        return composition.registerSceneConfigurationEditor(
            {{},
             cxx::make_basic_type_info<::lux::engine::render::RenderSystemConfiguration>(),
             +[](EditorContext& context,
                 std::span<const std::byte> bytes) noexcept -> std::unique_ptr<SceneConfigurationForm>
             { return std::make_unique<RenderConfigurationEditor>(context, bytes); }}
        );
    }
} // namespace lux::editor
