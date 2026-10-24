#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/UiTargetImage.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/FontAtlas.hpp>
#include <memory>

namespace lux::rendering
{
    class RenderExecution;
    struct FrameInput;
} // namespace lux::rendering

namespace lux::editor
{
    class EditorWindow;

    // One window's independent Render Scene. No empty Engine World, simulation, thread or frame ring.
    // All requests use the application's existing Main/Asset clients and execution resource.
    class EditorUiScene final
    {
        struct ImageRemoval
        {
            ImageRemoval* next = nullptr;
            rdesc::UiImageToken token;
            std::uint64_t revision = 0;
            void (*complete)(ImageRemoval&, FrameworkResult<void>) noexcept = nullptr;
            std::shared_ptr<const void> code_lifetime;
        };

    public:
        class RemovalSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(FrameworkResult<void>)>;

            template <class Receiver> struct Operation final : ImageRemoval
            {
                using operation_state_concept = stdexec::operation_state_t;
                EditorUiScene& scene;
                Receiver receiver;

                Operation(EditorUiScene& scene, rdesc::UiImageToken token, Receiver receiver) noexcept
                    : scene(scene), receiver(std::move(receiver))
                {
                    this->token = token;
                    code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    complete = [](ImageRemoval& node, FrameworkResult<void> result) noexcept
                    {
                        auto& operation = static_cast<Operation&>(node);
                        stdexec::set_value(std::move(operation.receiver), std::move(result));
                    };
                }

                void start() & noexcept
                {
                    scene.beginImageRemoval(*this);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{scene_, token_, std::move(receiver)};
            }

        private:
            friend class EditorUiScene;

            RemovalSender(EditorUiScene& scene, rdesc::UiImageToken token) noexcept : scene_(scene), token_(token) {}

            EditorUiScene& scene_;
            rdesc::UiImageToken token_;
        };

        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorUiScene>> create(
            rendering::RenderExecution&,
            EditorWindow&,
            render::TargetDesc,
            render::SurfaceDescription,
            ui::FontAtlas
        ) noexcept;
        ~EditorUiScene() noexcept;
        EditorUiScene(const EditorUiScene&) = delete;
        EditorUiScene& operator=(const EditorUiScene&) = delete;

        // Record desired window state; zero/minimized extents disable presentation until restored.
        void setExtent(std::uint32_t width, std::uint32_t height, bool active) noexcept;
        [[nodiscard]] FrameworkResult<rdesc::UiImageToken> addTargetImage(
            render::TargetId,
            render::TargetDesc
        ) noexcept;

        // The terminal result certifies only this token's old sampling relation; retain its Target until success.
        [[nodiscard]] RemovalSender removeTargetImage(rdesc::UiImageToken token) noexcept
        {
            return RemovalSender{*this, token};
        }

        [[nodiscard]] bool imageReady(rdesc::UiImageToken) const noexcept;
        [[nodiscard]] FrameworkResult<bool> outputReady() const noexcept;
        [[nodiscard]] render::RenderError configurationError() const noexcept;
        [[nodiscard]] FrameworkResult<void> appendFrame(rendering::FrameInput&) noexcept;
        void stopFrames() noexcept;
        // Only protocol work has settled. Native-window lifetime ends at successful RemoveTarget
        // or after RenderExecution shutdown; an error never claims an unavailable response succeeded.
        [[nodiscard]] bool settled() const noexcept;

    private:
        struct Impl;
        void beginImageRemoval(ImageRemoval&) noexcept;
        static void deliverImageRemoval(ImageRemoval&, FrameworkResult<void>) noexcept;
        explicit EditorUiScene(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
