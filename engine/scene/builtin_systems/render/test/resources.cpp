#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

void verifyRenderContext();

int main()
{
    using namespace lux::render;
    using namespace lux::scene;
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
    auto execution = lux::process::ExecutionRuntime::create({1, 64, 64, {64}});
    assert(execution);
    lux::process::TaskScope tasks{*execution};
    constexpr std::size_t capacity = 256;
    auto made_resources = lux::scene::RenderResources::create(*runtime, tasks, execution->cpu(), {capacity});
    assert(made_resources);
    auto resources = std::move(*made_resources);
    const auto pump = [&] {
        std::size_t controls = 2, programs = 1;
        const auto adopted = runtime->collectCompletions(3);
        assert(runtime->submitPending(controls, programs));
        assert(adopted && *adopted <= 3 && controls <= 2 && programs <= 1);
        if (resources)
            assert(execution->collectCompletions());
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

    // Admission reserves obligations before side effects. A full fixed set of
    // cancelled, unstarted records retires in one poll, without a command quota.
    std::vector<RenderResourceId> cancelled;
    for (std::size_t index{}; index != capacity; ++index)
    {
        auto admitted = resources->requestScene({.name = "cancelled before submission"}, {});
        assert(admitted);
        cancelled.push_back(*admitted);
    }
    auto full = resources->requestScene({}, {});
    assert(!full && full.error().code == ERendererError::CAPACITY);
    const auto unstarted = resources->sceneReceipt(cancelled.front());
    for (auto id : cancelled)
        resources->release(id);
    assert(execution->collectCompletions());
    assert(resources->empty());
    assert(unstarted.status().state == ESceneResourceState::RETIRED && !unstarted.status().scene.isValid());
    assert(!resources->retain(cancelled.front()));

    // No window, CPU UI or SceneInstance is involved. Vulkan really creates
    // the Scene; the reply remains unadopted when its business owner releases it.
    auto late = resources->requestScene({.name = "late CPU owner"}, {});
    assert(late);
    auto late_receipt = resources->sceneReceipt(*late);
    std::size_t commands = 1, programs = 0;
    assert(runtime->submitPending(commands, programs));
    assert(execution->collectCompletions());
    assert(late_receipt.status().state == ESceneResourceState::CREATING);
    resources->release(*late);
    until([&] { return late_receipt.status().state == ESceneResourceState::RETIRED; });
    assert(late_receipt.status().scene.isValid() && late_receipt.status().failure.ok());

    // Successful late attachment is still reclaimed once; the observation
    // retains the result but neither the plugin's code nor the resource use.
    const auto& feature = lux::render::kViewCameraRenderFeatureRegistration;
    assert(runtime->beginFeatureRegistration({feature}));
    until([&] { return runtime->featureRegistrationStatus().state == EFeatureRegistrationState::READY; });
    assert(runtime->commitFeatureRegistration());
    std::vector<std::byte> defaults, wire;
    assert(feature.configuration.portable.encode_default(defaults));
    assert(feature.configuration.materialize_attach(defaults, wire));
    auto code = std::make_shared<int>(1);
    std::weak_ptr<int> code_observer = code;
    auto attached = resources->requestScene(
        {.name = "late successful attachment"},
        {{feature.factory.descriptor.type,
          runtime->features().find(feature.factory.descriptor.type)->feature_type_id,
          std::move(wire),
          code}}
    );
    assert(attached);
    code.reset();
    const auto attached_receipt = resources->sceneReceipt(*attached);
    until([&] { return attached_receipt.status().state == ESceneResourceState::ATTACHING; });
    assert(!code_observer.expired());
    resources->release(*attached);
    until([&] { return attached_receipt.status().state == ESceneResourceState::RETIRED; });
    assert(attached_receipt.status().failure.ok() && code_observer.expired());

    auto survivor = resources->requestScene({.name = "independent Scene"}, {});
    assert(survivor);
    until([&] { return resources->sceneReceipt(*survivor).status().state == ESceneResourceState::READY; });
    const auto stable_identity = resources->sceneReceipt(*survivor).status().scene;
    assert(resources->retain(*survivor));
    resources->release(*survivor);
    assert(!resources->mesh(*survivor) && !resources->status(*survivor));

    auto control = runtime->control();
    assert(control);
    auto invalid_layer = control->get().setLayer({}, 0, stable_identity, {});
    until([&] { return invalid_layer.isReady(); });
    const auto rejected_layer = invalid_layer.tryResult();
    assert(rejected_layer && rejected_layer->get().code != 0);
    assert(rejected_layer->get().error.type == renderError<err::comm::RequestInvalid>().type);

    auto early_view = resources->requestView(*survivor, {.extent = {80, 60}});
    assert(early_view);
    const auto early_close = *resources->viewReceipt(*early_view);
    assert(execution->collectCompletions()); // Admit a real asynchronous create before releasing its owner.
    resources->release(*early_view);
    const auto stale_receipt = resources->viewReceipt(*early_view);
    assert(!stale_receipt && stale_receipt.error().code == ERendererError::STALE_VIEW);
    until([&] { return early_close.status().status.state == EViewState::CLOSED; });
    assert(resources->sceneReceipt(*survivor).status().state == ESceneResourceState::READY);

    auto image_view = resources->requestView(*survivor, {.extent = {64, 48}});
    assert(image_view);
    until([&] { return resources->observeView(*image_view)->status.state == EViewState::READY; });
    auto image = resources->viewOutput(*image_view);
    assert(!image && image.error().code == ERendererError::NOT_READY);
    const auto image_close = *resources->viewReceipt(*image_view);
    resources->release(*image_view);
    until([&] { return image_close.status().status.state == EViewState::CLOSED; });

    auto failed = resources->requestScene({.name = "invalid Feature"}, {{17, 0xFFFFFF, {}, {}}});
    assert(failed);
    auto failure_receipt = resources->sceneReceipt(*failed);
    until([&] { return failure_receipt.status().state == ESceneResourceState::ATTACHING; });
    // The attachment is in flight; destroy its CPU owner before its reply.
    resources->release(*failed);
    until([&] { return failure_receipt.status().state == ESceneResourceState::RETIRED; });
    const auto failure = failure_receipt.status();
    assert(failure.failure.type == renderError<err::feature::TypeNotRegistered>(0xFFFFFF).type);
    assert(failure.feature == 17 && failure.request != 0);
    assert(
        resources->sceneReceipt(*survivor).status().scene == stable_identity &&
        resources->sceneReceipt(*survivor).status().state == ESceneResourceState::READY
    );

    // A Program attachment, unlike the read-only receipt, holds a use. The
    // bounded idle rotation must release it even if no more frames are drawn.
    auto receipt = resources->sceneReceipt(*survivor);
    TRenderProgram<> input;
    RenderProgramSession::Builder builder(input);
    builder.begin({});
    input.kind = ERenderProgramKind::STATE_UPDATE;
    auto captured = resources->capture(std::array{*survivor, *survivor});
    assert(captured);
    builder.emplaceAttachment<RenderSubmissionState>(attachment_types::SubmissionState, std::move(*captured));
    assert(*runtime->submit(input) == EFrameSubmit::SUBMITTED);
    resources->release(*survivor);
    until([&] { return receipt.status().state == ESceneResourceState::RETIRED; });
    assert(receipt.status().failure.ok());
    assert(resources->empty());
    resources.reset();
    assert(runtime->statistics().frames == 0); // Retirement did not fabricate a draw.
    assert(runtime->statistics().validation_errors == 0);

    assert(tasks.join());
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
    verifyRenderContext();
    assert(failure_receipt.status().failure.type == failure.failure.type);
    assert(receipt.status().state == ESceneResourceState::RETIRED);
    std::cout
        << "PASS actual Vulkan Scene/View creation, late owner release, unproduced image refusal, Feature failure, "
           "independent Scene, Program use retirement; no draw submitted\n";
}
