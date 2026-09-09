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
        // Two independently acquired records distinguish releasing the overwritten packet
        // from transferring its replacement. No packet in this scope is submitted.
        {
            auto replacement_image = view.acquireImage();
            assert(replacement_image && !rendering::detail::ViewImageAccess::sameRecord(old, *replacement_image));
            const auto old_references = rendering::detail::ViewImageAccess::references(old);
            const auto new_references = rendering::detail::ViewImageAccess::references(*replacement_image);
            const auto accepted_before = renderer.statistics().accepted_frames;
            auto destination = seal(old);
            auto source = seal(*replacement_image);
            const auto replacement_sequence = source.sequence();
            assert(destination.sequence() != replacement_sequence);
            assert(rendering::detail::ViewImageAccess::references(old) == old_references + 1);
            assert(rendering::detail::ViewImageAccess::references(*replacement_image) == new_references + 1);
            destination = std::move(source);
            assert(destination.valid() && destination.sequence() == replacement_sequence);
            assert(!source.valid() && source.sequence() == 0);
            assert(rendering::detail::ViewImageAccess::references(old) == old_references);
            assert(rendering::detail::ViewImageAccess::references(*replacement_image) == new_references + 1);
            destination = {};
            assert(rendering::detail::ViewImageAccess::references(*replacement_image) == new_references);
            assert(renderer.statistics().accepted_frames == accepted_before);
            assert(renderer.imageEvidence(*replacement_image)->evidence == rendering::EImageEvidence::REQUESTED);
            assert(rendering::detail::ViewImageAccess::record(*replacement_image)->submitted.load() == 0);
            std::puts("packet move overwrite PASS: old references released, source transferred, no submission");
        }
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
#if defined(LUX_EDITOR_DIAGNOSTICS)
        const auto first_resize = view.status().request_sequence;
        while (rendering::detail::RendererTestAccess::inFlightResize(view) != first_resize)
        {
            poll();
            assert(view.status().acknowledged_sequence == acknowledged);
        }
#else
        for (unsigned i = 0; i != 8; ++i)
            poll();
#endif
        assert(view.requestExtent({256, 128}));
        const auto latest_sequence = view.status().request_sequence;
#if defined(LUX_EDITOR_DIAGNOSTICS)
        bool observed_old_reply{};
        assert(first_resize < latest_sequence);
#endif
        while (view.status().state != rendering::EViewState::READY)
        {
            poll();
            assert((view.status().requested_extent == rendering::PixelExtent{256, 128}));
#if defined(LUX_EDITOR_DIAGNOSTICS)
            if (view.status().acknowledged_sequence == first_resize)
            {
                observed_old_reply = true;
                assert((view.status().ready_extent == rendering::PixelExtent{192, 96}));
                assert(view.status().state == rendering::EViewState::RESIZING);
                assert(!view.acquireImage());
            }
#endif
            if (view.status().acknowledged_sequence == latest_sequence)
                assert((view.status().ready_extent == rendering::PixelExtent{256, 128}));
        }
        assert(view.status().acknowledged_sequence == latest_sequence);
#if defined(LUX_EDITOR_DIAGNOSTICS)
        assert(observed_old_reply);
        std::printf("late resize PASS admitted=%llu desired=%llu old reply observed before latest ack\n", first_resize,
                    latest_sequence);
        std::fflush(stdout);
#endif
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
#if defined(LUX_EDITOR_DIAGNOSTICS)
        std::weak_ptr<rendering::detail::ImageVersion> closing_version =
            rendering::detail::ViewImageAccess::record(*restored)->version;
        auto final_packet = seal(*restored);
        submit(final_packet);
        for (;;)
        {
            const auto evidence = renderer.imageEvidence(*restored);
            assert(evidence);
            if (evidence->evidence != rendering::EImageEvidence::REQUESTED)
                break;
            poll();
        }
        restored = lux::cxx::unexpected(rendering::RendererFailure{});
        pane.texture = {};
        auto runtime = renderer.acquire();
        assert(runtime);
        const auto frames_before_recycle = renderer.statistics().frames;
        // Recycle the actual bounded request slots with empty StateUpdates. They
        // release retained CPU attachments without submitting GPU frames or advancing fences.
        for (unsigned recycle = 0; recycle != 8; ++recycle)
        {
            lux::render::RenderProgram<> state_update;
            while (!runtime->programs().trySubmitPrepared(state_update))
                poll();
            poll();
        }
        while (closing_version.use_count() != 1)
        {
            assert(Clock::now() < deadline);
            // While READY this poll does not enqueue maintenance frames.
            assert(renderer.poll(1));
            std::this_thread::yield();
        }
        const auto closing_submission = closing_version.lock()->last_submission.load();
        assert(renderer.statistics().frames == frames_before_recycle);
        const auto completed_before_close = renderer.statistics().gpu_completed;
        const auto retired_before_close = renderer.statistics().descriptors_retired;
        assert(closing_submission > completed_before_close);
        assert(view.beginClose());
        assert(*view.advanceClose() == rendering::ERenderClose::PENDING);
        assert(renderer.poll(1));
        assert(renderer.statistics().descriptors_retired == retired_before_close);
        while (*view.advanceClose() != rendering::ERenderClose::COMPLETE)
            poll();
        assert(closing_version.expired());
        assert(renderer.statistics().gpu_completed >= closing_submission);
        std::printf("GPU close PASS cpu_references=1 submitted=%llu completed_before=%llu completed_after=%llu\n",
                    closing_submission, completed_before_close, renderer.statistics().gpu_completed);
#endif
        registration->reset();
        window.uiSession().clearSplitLayout();
    }
} // namespace
