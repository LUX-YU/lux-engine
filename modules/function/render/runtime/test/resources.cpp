#include <lux/engine/render/RenderRuntime.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace lux::render;
    using namespace std::chrono_literals;
    RendererConfig config;
    config.validation = true;
    auto diagnostics = [](auto severity, auto message) {
        if (severity == 2)
        {
            std::cerr << message << '\n';
        }
    };
    auto created = RenderRuntime::create(std::move(config), std::move(diagnostics));
    if (!created)
    {
        std::cerr << "Runtime startup failed: " << static_cast<unsigned>(created.error().code) << '\n';
        return 1;
    }
    auto runtime = std::move(*created);
    const auto pump = [&] {
        std::size_t controls = 2, programs = 1;
        const auto adopted = runtime->poll(3, controls, programs);
        assert(adopted && *adopted <= 3 && controls <= 2 && programs <= 1);
    };
    const auto until = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
            {
                return;
            }
            pump();
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "The requested completion did not arrive");
    };

    // No window, CPU UI or SceneInstance is involved. Vulkan really creates
    // the Scene; zero Main adoption budget retains the late result's record.
    auto late = runtime->createScene({.name = "late CPU owner"}, {});
    assert(late);
    auto late_receipt = late->receipt();
    std::size_t commands = 1, programs = 0;
    assert(runtime->poll(0, commands, programs));
    assert(commands == 0);
    assert(late_receipt.status().state == ESceneResourceState::CREATING);
    *late = {};
    until([&] { return late_receipt.status().state == ESceneResourceState::RETIRED; });
    assert(late_receipt.status().scene.isValid() && late_receipt.status().failure.ok());

    auto survivor = runtime->createScene({.name = "independent Scene"}, {});
    assert(survivor);
    until([&] { return survivor->status().state == ESceneResourceState::READY; });
    const auto stable_identity = survivor->id();

    auto early_view = runtime->openView(*survivor, {.extent = {80, 60}});
    assert(early_view);
    commands = 1;
    assert(runtime->poll(0, commands, programs));
    assert(commands == 0);
    early_view->reset(); // No explicit drain needed in a View facade destructor.
    until([&] { return runtime->statistics().views == 0; });
    assert(survivor->status().state == ESceneResourceState::READY);

    auto image_view = runtime->openView(*survivor, {.extent = {64, 48}});
    assert(image_view);
    until([&] { return (*image_view)->status().state == EViewState::READY; });
    auto image = (*image_view)->acquireImage();
    assert(image && image->lease.valid());
    const auto image_evidence = runtime->imageEvidence(*image);
    assert(image_evidence && image_evidence->evidence == EImageEvidence::REQUESTED);
    image_view->reset();
    for (unsigned i = 0; i < 6; ++i)
    {
        pump();
    }
    assert(runtime->statistics().views == 1); // A real CPU image borrow still exists.
    *image = {};
    until([&] { return runtime->statistics().views == 0; });

    auto failed = runtime->createScene({.name = "invalid Feature"}, {{17, 0xFFFFFF, {}, {}}});
    assert(failed);
    auto failure_receipt = failed->receipt();
    until([&] { return failure_receipt.status().state == ESceneResourceState::ATTACHING; });
    // The attachment is in flight; destroy its CPU owner before its reply.
    *failed = {};
    until([&] { return failure_receipt.status().state == ESceneResourceState::RETIRED; });
    const auto failure = failure_receipt.status();
    assert(failure.failure.type == renderError<err::feature::TypeNotRegistered>(0xFFFFFF).type);
    assert(failure.feature == 17 && failure.request != 0);
    assert(survivor->id() == stable_identity && survivor->status().state == ESceneResourceState::READY);

    // A Program attachment, unlike the read-only receipt, holds a use. The
    // bounded idle rotation must release it even if no more frames are drawn.
    auto receipt = survivor->receipt();
    RenderProgram<> input;
    RenderProgramSession::Builder builder(input);
    builder.begin({});
    input.kind = ERenderProgramKind::StateUpdate;
    builder.emplaceAttachment<RenderSceneLease>(901, survivor->retain());
    assert(*runtime->submit(input) == EFrameSubmit::SUBMITTED);
    *survivor = {};
    until([&] { return receipt.status().state == ESceneResourceState::RETIRED; });
    assert(receipt.status().failure.ok());
    assert(runtime->statistics().runtime_leases == 0);
    assert(runtime->statistics().frames == 0); // Retirement did not fabricate a draw.
    assert(runtime->statistics().validation_errors == 0);

    assert(runtime->beginClose());
    bool complete{};
    until([&] {
        std::size_t replies = 8, controls = 4, programs = 1;
        auto closing = runtime->advanceClose(replies, controls, programs);
        assert(closing);
        complete = *closing == ERenderClose::COMPLETE;
        return complete;
    });
    assert(runtime->joinStopped());
    assert(runtime->status().state == ERenderRuntimeState::RETIRED);
    // Retained receipts remain observations after the runtime is destroyed.
    runtime.reset();
    assert(failure_receipt.status().failure.type == failure.failure.type);
    assert(receipt.status().state == ESceneResourceState::RETIRED);
    std::cout << "PASS actual Vulkan Scene/View creation, late owner release, CPU image guard, Feature failure, "
                 "independent Scene, Program use retirement; no draw submitted\n";
}
