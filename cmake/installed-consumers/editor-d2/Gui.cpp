#include <chrono>
#include <consumer.inspector.generated.hpp>
#include <consumer/Gui.hpp>
#if defined(CONSUMER_WIDGET_DIAGNOSTICS)
#include <InspectorWidget.hpp>
#include <cassert>
#include <cstdio>
#include <list>
#endif

namespace consumer
{
    void checkCompletedGesture(lux::editor::scene::SceneEditor &, lux::world::WorldObjectId, lux::ui::Frame &);
    bool drawOmissionProbe(lux::editor::scene::SceneEditor &, lux::world::WorldObjectId, lux::ui::Frame &,
                           lux::editor::gui::InspectorInteraction &);

    namespace
    {
        std::size_t draws{};
        DrawSample sample;
        bool sampling{};

#if defined(CONSUMER_WIDGET_DIAGNOSTICS)
        struct CountedList final
        {
            struct Iterator final
            {
                using iterator_category = std::forward_iterator_tag;
                using value_type = int;
                using difference_type = std::ptrdiff_t;
                using reference = int &;
                using pointer = int *;
                std::list<int>::iterator current;
                std::size_t *steps{};
                int &operator*() const
                {
                    return *current;
                }
                Iterator &operator++()
                {
                    ++current;
                    ++*steps;
                    return *this;
                }
                Iterator operator++(int)
                {
                    auto before = *this;
                    ++*this;
                    return before;
                }
                bool operator==(const Iterator &other) const
                {
                    return current == other.current;
                }
            };
            std::list<int> values = std::list<int>(4096, 42);
            std::size_t steps{};
            Iterator begin()
            {
                return {values.begin(), &steps};
            }
            Iterator end()
            {
                return {values.end(), &steps};
            }
            std::size_t size() const
            {
                return values.size();
            }
        };

        void checkPageAnchor(lux::editor::gui::InspectorInteraction &state)
        {
            using namespace lux::editor::gui::generated_support;
            ImGui::PushID("diagnostic-page-anchor");
            CountedList list;
            const auto previous = std::exchange(state.read_only, true);
            state.input<PageBounds>(ImGui::GetID("container-page")).first = 4032;
            auto page = containerPage(list, state);
            assert(page.first == 4032 && page.end == 4096 && list.steps == 4032);
            list.steps = 0;
            for (unsigned draw = 0; draw < 5; ++draw)
            {
                page = containerPage(list, state);
                auto item = page.begin;
                for (auto index = page.first; index < page.end; ++index, ++item)
                {
                    assert(*item == 42);
                }
            }
            assert(list.steps == 5 * 64);
            std::list<int> replacement(4096, 73);
            list.values.swap(replacement);
            state.invalidateContainerIterators();
            list.steps = 0;
            page = containerPage(list, state);
            assert(list.steps == 4032 && *page.begin == 73);
            state.read_only = previous;
            state.invalidateContainerIterators();
            ImGui::PopID();
            std::puts("DIAGNOSTIC list page: size=4096 first=4032 visible=64 initial_seek=4032 "
                      "idle_5_draws_steps=320 replacement_reseek=4032 readonly_navigation=1");
        }

        void checkGestureRetention(lux::editor::scene::SceneEditor &document, lux::world::WorldObjectId object,
                                   lux::ui::Frame &frame)
        {
            using namespace lux::editor;
            gui::InspectorInteraction interaction(document, "diagnostic-gesture");
            const auto before = document.historyView()->history;
            std::size_t busy{};
            auto connection = document.observeScoped<scene::SceneEditor::componentChanged>(
                [&](const scene::ComponentNotice &) noexcept
                {
                    assert(interaction.active());
                    assert(!interaction.finish(document));
                    assert(interaction.failure().code == editing::EEditError::BUSY);
                    assert(interaction.active());
                    ++busy;
                });
            const auto access = [](auto &component) noexcept { return &component.settings.gain; };
            interaction.field<consumer::Component, double>(
                document, object, frame, "settings.gain.diagnostic", "Diagnostic gain", access,
                [](double &value, auto &state)
                {
                    assert(state.beforeWrite());
                    value = 2.0;
                    return lux::ui::EditResult{true, true, false, false};
                },
                false);
            assert(busy == 1 && interaction.active());
            connection.reset();
            assert(interaction.finish(document) && !interaction.active());
            assert(document.historyView()->history.cursor == before.cursor + 1);
            assert(document.undo() && document.historyView()->history.current == before.current);
            interaction.field<consumer::Component, double>(
                document, object, frame, "settings.gain.cancel", "Diagnostic cancel", access,
                [](double &value, auto &state)
                {
                    assert(state.beforeWrite());
                    value = 3.0;
                    return lux::ui::EditResult{true, true, false, false};
                },
                false);
            assert(interaction.active() && interaction.finish(document));
            assert(document.undo());
            assert(document.historyView()->history.current == before.current);
            std::puts("DIAGNOSTIC Inspector gesture: reentrant commit BUSY retains token; same gesture commits once; "
                      "Undo restores StateId; completed edit remains undoable");
        }
#endif

        void draw(lux::editor::scene::SceneEditor &document, lux::world::WorldObjectId object, lux::ui::Frame &frame,
                  lux::editor::gui::InspectorInteraction &interaction)
        {
            ++draws;
            if (drawOmissionProbe(document, object, frame, interaction))
            {
                return;
            }
            if (draws == 1 && !document.summary().read_only)
            {
                checkCompletedGesture(document, object, frame);
            }
#if defined(CONSUMER_WIDGET_DIAGNOSTICS)
            if (draws == 1)
            {
                checkPageAnchor(interaction);
                checkGestureRetention(document, object, frame);
            }
#endif
            const auto begin = std::chrono::steady_clock::now();
            lux::editor::gui::generated::consumerBindings().front().draw(document, object, frame, interaction);
            if (sampling && sample.warmup < 20)
            {
                ++sample.warmup;
            }
            else if (sampling && sample.draws < 100)
            {
                ++sample.draws;
                sample.active_microseconds +=
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
            }
        }
    } // namespace

    lux::editor::gui::ComponentBinding binding()
    {
        auto result = lux::editor::gui::generated::consumerBindings().front();
        result.draw = draw;
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
