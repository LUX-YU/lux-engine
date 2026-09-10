#pragma once

namespace
{
    using namespace lux::editor;
    // Two real Sessions, two real Scenes and independently compiled graphs. The overlay is first
    // measured alone, then moved onto the base target as its second content layer.
    bool exercisePreserveGpu(
        rendering::EditorRenderer &renderer, ui::EditorWindow &window, lux::process::ExecutionRuntime &execution,
        sessions::SceneOpenInfo &input, lux::scene::RenderRuntimeLease &runtime,
        const rendering::ViewImage &base, const std::filesystem::path &output)
    {
        auto opened = sessions::SceneSession::openInspection(input);
        require(opened && !input.scene, "preserve overlay Session owns its independent input");
        auto overlay = std::move(*opened);
        auto created = sessions::SceneView::create(window.uiSession().dispatcherRef(), *overlay, renderer);
        require(created, "preserve overlay View factory");
        auto view = std::move(*created);
        require(view->requestExtent(base.extent), "equal physical extent for compositing");
        ImagePane pane(window.uiSession().dispatcherRef());
        pane.texture = base.texture;
        auto registration = window.uiSession().registerPane(pane);
        require(registration, "preserve image Pane registration");
        window.uiSession().setSplitLayout({{}, "lifetime.image", {}, {}, 0, 0, 0, {}});
        const auto deadline = Clock::now() + std::chrono::seconds{30};
        std::uint64_t cycle{};
        rendering::ViewImage overlay_image;
        const auto step = [&] {
            require(Clock::now() < deadline && execution.drainMain(64) && renderer.poll(64),
                    "preserve finite owner progress");
            const sessions::SceneOwnerUpdate update{++cycle, 1.0 / 60.0};
            require(overlay->updateAtOwnerSafePoint(update) && view->synchronize(), "preserve overlay owner update");
            if (overlay_image.lease.valid())
            {
                auto current = view->image();
                require(current, "preserve current overlay camera image");
                overlay_image = std::move(*current);
            }
            require(window.beginFrame({{1600, 900}, 1.0F / 60, {1, 1}}) && window.drawPanes(),
                    "preserve real UI frame");
            auto snapshot = window.finishFrame();
            require(snapshot, "preserve owning UI snapshot");
            const std::array images{base, overlay_image};
            auto packet = renderer.sealFrame(*snapshot, {images.data(), overlay_image.lease.valid() ? 2U : 1U});
            require(packet && !snapshot->valid(), "preserve frame retains actual target image");
            while (packet->valid())
            {
                require(Clock::now() < deadline && renderer.trySubmitFrame(*packet) && renderer.poll(64),
                        "preserve frame admission retains backpressured input");
                std::this_thread::yield();
            }
            require(overlay->advanceScene(update), "preserve overlay advances once per owner cycle");
        };
        for (;;)
        {
            step();
            const auto rows = overlay->readResources();
            require(rows, "preserve actual resource snapshot");
            if ((*rows)->rows.size() == 1 && (*rows)->rows.front().state == sessions::ESceneResourceState::READY &&
                view->image())
                break;
        }
        require(view->synchronize(), "preserve camera prepared after actual View readiness");
        overlay_image = *view->image();
        const auto *base_record = rendering::detail::ViewImageAccess::record(base);
        const auto *overlay_record = rendering::detail::ViewImageAccess::record(overlay_image);
        const auto base_target = base_record->version->target;
        const auto overlay_target = overlay_record->version->target;
        const auto overlay_scene = overlay_record->version->scene;
        const auto overlay_view = overlay_record->version->view;
        require(base_target != overlay_target && base_record->version->scene != overlay_scene,
                "preserve uses distinct Scenes and physical target owners");
        const auto capture = [&](const rendering::ViewImage &image, const char *name) {
            for (unsigned warmup = 0; warmup != 16; ++warmup)
                step();
            Readback result;
            result.start(runtime, image);
            while (!result.request.isReady())
                step();
            static_cast<void>(result.finish(output / name));
            return std::move(result.pixels);
        };
        const auto first = capture(base, "preserve-base.ppm");
        const auto second = capture(overlay_image, "preserve-overlay.ppm");
        // Remove the previous target binding before changing this Scene's preserve format key.
        runtime.control().removeLayer(overlay_target, 0);
        runtime.control().setLayer(base_target, 1, overlay_scene, overlay_view);
        const auto combined = capture(base, "preserve-combined.ppm");
        require(first.size() == second.size() && first.size() == combined.size() && first.size() >= 4,
                "preserve actual readbacks have identical byte extent");
        std::size_t background{}, retained_content{}, overlaid_content{}, mismatches{};
        for (std::size_t pixel = 0; pixel != first.size(); pixel += 4)
        {
            const auto same = [&](const auto &left, std::size_t a, const auto &right, std::size_t b) {
                return std::equal(left.begin() + a, left.begin() + a + 4, right.begin() + b);
            };
            const bool empty_overlay = same(second, pixel, second, 0);
            if (empty_overlay)
            {
                ++background;
                retained_content += !same(first, pixel, second, 0) && same(combined, pixel, first, pixel);
            }
            else
                overlaid_content += !same(first, pixel, second, pixel) && same(combined, pixel, second, pixel);
            mismatches += !same(combined, pixel, empty_overlay ? first : second, pixel);
        }
        const bool correct = background > 100 && retained_content > 100 && overlaid_content > 100 && mismatches == 0;
        std::printf("preserve GPU base_target=%u:%u overlay_target=%u:%u scene=%u:%u "
                    "background=%zu retained_content=%zu overlaid_content=%zu pixel_mismatches=%zu correct=%u\n",
                    base_target.index, base_target.gen, overlay_target.index, overlay_target.gen,
                    overlay_scene.index, overlay_scene.gen, background, retained_content, overlaid_content,
                    mismatches, unsigned(correct));
        std::vector<char> graph_text(1024 * 1024);
        auto dump = runtime.control().dumpRenderGraph(overlay_scene, graph_text.data(), graph_text.size());
        require(dump.valid(), "preserve actual graph dump admission");
        while (!dump.isReady())
            step();
        const auto graph = dump.tryResult();
        require(graph && graph->get().status == 0 && graph->get().written == graph->get().needed,
                "preserve complete graph dump");
        std::ofstream graph_file(output / "preserve-graph.txt", std::ios::binary);
        graph_file.write(graph_text.data(), graph->get().written);
        require(graph_file.good(), "preserve compiled graph evidence");
        runtime.control().removeLayer(base_target, 1);
        const auto restored = capture(base, "preserve-restored.ppm");
        const bool restoration = restored == first;
        std::printf("preserve removed_layer_restores_base=%u cycles=%llu\n", unsigned(restoration), cycle);
        overlay_image = {};
        pane.texture = {};
        require(view->beginClose() && overlay->beginClose(), "preserve explicit View and Session close");
        while (view || overlay)
        {
            require(Clock::now() < deadline && execution.drainMain(64) && renderer.poll(64),
                    "preserve close progress retains owners");
            if (view)
            {
                const auto closed = view->advanceClose();
                require(closed, "preserve View close result");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    view.reset();
            }
            if (overlay)
            {
                const auto closed = overlay->advanceClose();
                require(closed, "preserve Session close result");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    overlay.reset();
            }
        }
        registration->reset();
        window.uiSession().clearSplitLayout();
        std::puts("preserve overlay View and Session explicitly COMPLETE; base owner remains live");
        return correct && restoration;
    }
}
