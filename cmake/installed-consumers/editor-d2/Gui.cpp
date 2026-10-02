#include <imgui.h>
#include <lux/engine/ui/Element.hpp>
#include <chrono>
#include <consumer.inspector.generated.hpp>
#include <consumer/Gui.hpp>

namespace consumer
{
    namespace
    {
        std::size_t draws{};
        DrawSample sample;
        bool sampling{};

        class MeasuredElement final : public lux::ui::Element
        {
        public:
            MeasuredElement(lux::ui::Element& parent, lux::ui::ElementId id) : lux::ui::Element(parent, std::move(id))
            {}
            std::unique_ptr<lux::ui::Element> content;

        private:
            lux::ui::SizeHint sizeHintContent() noexcept override
            {
                return content->sizeHint();
            }
            lux::ui::SizeHint measureContent(float width) noexcept override
            {
                return content->measure(width);
            }
            void arrangeContent() noexcept override
            {
                content->arrange({{}, rect().size});
            }
            void draw() noexcept override
            {
                ++draws;
                const auto begin = std::chrono::steady_clock::now();
                drawChild(*content);
                if (sampling && sample.warmup < 20)
                    ++sample.warmup;
                else if (sampling && sample.draws < 100)
                {
                    ++sample.draws;
                    sample.active_microseconds +=
                        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
                }
            }
        };

        lux::editor::scene::InspectorComponent::CreateResult create(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            lux::editor::scene::InspectorFields& fields
        ) noexcept
        {
            auto result = std::make_unique<MeasuredElement>(parent, std::move(id));
            auto content = lux::editor::scene::generated::consumerBindings().front().create(
                *result,
                lux::ui::ElementId{"generated"},
                fields
            );
            if (!content)
                return lux::cxx::unexpected(content.error());
            result->content = std::move(*content);
            return std::unique_ptr<lux::ui::Element>(std::move(result));
        }
    } // namespace

    lux::editor::scene::InspectorComponent binding()
    {
        auto result = lux::editor::scene::generated::consumerBindings().front();
        result.create = create;
        return result;
    }

    std::size_t drawCount() noexcept
    {
        return draws;
    }

    void beginDrawSample() noexcept
    {
        sample = {};
        sampling = true;
    }

    DrawSample drawSample() noexcept
    {
        return sample;
    }
} // namespace consumer
