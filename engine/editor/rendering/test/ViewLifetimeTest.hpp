#pragma once
#include <lux/engine/editor/rendering/detail/ViewImageLifetime.hpp>
#include <lux/engine/ui/ViewportElement.hpp>

namespace
{
    class ImagePane final : public lux::object::Object<ImagePane, lux::ui::Pane>
    {
    public:
        explicit ImagePane(lux::object::ObjectDispatcherRef dispatcher)
            : Object(dispatcher, lux::ui::PaneId{"lifetime.image"}, lux::ui::PaneTypeId{"lifetime"}, "Lifetime")
        {
        }
        lux::ui::TextureHandle texture;

    private:
        lux::ui::ViewportElement viewport_;
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            static_cast<void>(viewport_.draw(frame, {texture}));
        }
    };

    void exerciseViewLifetime(lux::editor::rendering::EditorRenderer &renderer, lux::editor::ui::EditorWindow &window,
                              lux::editor::rendering::RenderView &view)
    {
        using namespace lux::editor;
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::seconds{20};
        ImagePane pane(window.uiSession().dispatcherRef());
        auto registration = window.uiSession().registerPane(pane);
        assert(registration);
        window.uiSession().setSplitLayout({{}, "lifetime.image", {}, {}, 0, 0, 0, {}});
        const auto poll = [&]
        {
            assert(Clock::now() < deadline && renderer.poll(1));
            assert(view.status().state != rendering::EViewState::FAILED);
            std::this_thread::yield();
        };
        const auto seal = [&](const rendering::ViewImage &image)
        {
            pane.texture = image.texture;
            assert(window.beginFrame({{256, 256}, 1.0F / 60, {1, 1}}));
            assert(window.drawPanes());
            auto snapshot = window.finishFrame();
            assert(snapshot);
            auto packet = renderer.sealFrame(*snapshot, {&image, 1});
            assert(packet && !snapshot->valid());
            return std::move(*packet);
        };
        const auto submit = [&](rendering::EditorFramePacket &packet)
        {
            while (packet.valid())
            {
                assert(renderer.trySubmitFrame(packet));
                poll();
            }
        };
        auto old = *view.acquireImage();
        const auto old_extent = old.extent;
        const auto old_generation = old.content.source.surface_generation;
        const auto initial_descriptors = renderer.statistics().descriptors_created;
        for (unsigned i = 0; i != 40; ++i)
        {
            auto packet = seal(old);
            submit(packet);
        }
        while (renderer.statistics().descriptors_created < initial_descriptors + 2)
            poll();
        std::puts("lifetime: original target sampled in both FIF slots");
        std::fflush(stdout);
        auto pending = seal(old);
        const auto sequence = pending.sequence();
        const auto acknowledged = view.status().acknowledged_sequence;
        const auto retired = renderer.statistics().descriptors_retired;
        assert(view.requestExtent({192, 96}));
        for (unsigned i = 0; i != 32; ++i)
        {
            poll();
            assert(pending.valid() && pending.sequence() == sequence);
            assert(view.status().ready_extent == old_extent);
            assert(view.status().acknowledged_sequence == acknowledged);
            assert(renderer.statistics().descriptors_retired == retired);
        }
        // The admitted old packet must still resolve the old target storage while resize is pending.
        submit(pending);
        assert(!pending.valid() && renderer.imageEvidence(old));
        const auto *record = rendering::detail::ViewImageAccess::record(old);
        const auto backing_before = record->version->backing_revision.load();
        old = {};
        for (unsigned i = 0; i != 8; ++i)
            poll();
        assert(view.requestExtent({256, 128}));
        const auto latest_sequence = view.status().request_sequence;
        while (view.status().state != rendering::EViewState::READY)
        {
            poll();
            assert((view.status().requested_extent == rendering::PixelExtent{256, 128}));
            if (view.status().acknowledged_sequence == latest_sequence)
                assert((view.status().ready_extent == rendering::PixelExtent{256, 128}));
        }
        assert(view.status().acknowledged_sequence == latest_sequence);
        auto fresh = *view.acquireImage();
        assert((fresh.extent == rendering::PixelExtent{256, 128}));
        assert(fresh.content.source.surface_generation > old_generation);
        assert(rendering::detail::ViewImageAccess::record(fresh)->version->backing_revision.load() > backing_before);
        const auto descriptors_before = renderer.statistics().descriptors_created;
        for (unsigned i = 0; i != 40; ++i)
        {
            auto packet = seal(fresh);
            submit(packet);
        }
        while (renderer.statistics().descriptors_created < descriptors_before + 2)
            poll();
        assert(renderer.statistics().descriptors_retired >= retired + 2);
        fresh = {};
        assert(view.requestExtent({}));
        for (unsigned i = 0; i != 32; ++i)
        {
            poll();
            assert(view.status().state == rendering::EViewState::SUSPENDED);
            assert(!view.acquireImage());
        }
        assert(view.requestExtent({128, 64}));
        while (view.status().state != rendering::EViewState::READY)
            poll();
        auto restored = view.acquireImage();
        assert(restored && (restored->extent == rendering::PixelExtent{128, 64}));
        assert(renderer.statistics().texture_misses == 0 && renderer.statistics().validation_errors == 0);
        std::printf("lifetime PASS held_packet_steps=32 zero_extent_steps=32 generations=%llu/%llu "
                    "backing=%llu/%llu descriptors=%llu/%llu\n",
                    old_generation, restored->content.source.surface_generation, backing_before,
                    rendering::detail::ViewImageAccess::record(*restored)->version->backing_revision.load(),
                    renderer.statistics().descriptors_created, renderer.statistics().descriptors_retired);
        registration->reset();
        window.uiSession().clearSplitLayout();
    }
} // namespace
