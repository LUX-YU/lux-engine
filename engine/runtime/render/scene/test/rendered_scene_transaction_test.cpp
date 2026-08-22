#include <lux/engine/runtime/render/scene/RenderedSceneComposition.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/scene/SceneAsset.hpp>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    bool check(bool condition, const char* message)
    {
        std::printf("[%s] %s\n", condition ? " ok " : "FAIL", message);
        return condition;
    }

    [[nodiscard]] lux::asset::asset_id_t registerScene(
        lux::asset::AssetManager& assets,
        const char* id_text)
    {
        lux::scene::SceneDescription description;
        description.id = lux::asset::asset_id_t{
            uuids::uuid::from_string(id_text).value()};
        auto info = std::make_unique<lux::asset::AssetInfo>();
        info->id = description.id;
        info->type = lux::scene::kSceneAssetType;
        if (!assets.registerAsset(std::make_unique<lux::scene::SceneAsset>(
                std::move(info),
                std::make_unique<lux::scene::SceneDescription>(
                    description))))
        {
            return {};
        }
        return description.id;
    }

    template <class Reply>
    bool publishReply(
        const std::shared_ptr<lux::render::RenderControlChannel<>>& channel,
        const std::shared_ptr<lux::render::RenderChannelSync>& sync,
        lux::render::TypeId type_id,
        lux::render::RequestId request_id,
        const Reply& reply)
    {
        auto* packet = channel->responses.tryBeginWrite();
        if (!packet)
            return false;
        packet->clear_keep_capacity();
        packet->payload.resize(sizeof(Reply));
        std::memcpy(packet->payload.data(), &reply, sizeof(Reply));
        packet->replies.push_back(lux::render::ReplyRecord{
            .type_id = type_id,
            .payload_offset = 0u,
            .payload_size = static_cast<std::uint32_t>(sizeof(Reply)),
            .request_id = request_id});
        if (!channel->responses.publishWrite())
            return false;
        sync->notifyReplyProduced();
        return true;
    }

    bool popCommand(
        const std::shared_ptr<lux::render::RenderControlChannel<>>& channel,
        lux::render::OperationPacket<>& packet)
    {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (channel->requests.tryPop(packet) ==
                lux::cxx::EQueuePopResult::VALUE)
            {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    struct Fixture final
    {
        lux::asset::AssetManager assets{
            lux::asset::runtimeAssetCodecCatalog()};
        lux::runtime::testing::AssetAsyncTestServices async{assets};
        std::shared_ptr<lux::render::RenderChannelSync> sync{
            std::make_shared<lux::render::RenderChannelSync>()};
        std::shared_ptr<lux::render::RenderFrameChannel<>> frame_channel{
            lux::render::RenderFrameChannel<>::create()};
        std::shared_ptr<lux::render::RenderControlChannel<>> control_channel{
            lux::render::RenderControlChannel<>::create()};
        lux::render::RenderFrameSession frame{frame_channel, sync};
        lux::render::RenderControlSession control{control_channel, sync};
        lux::render::FeatureCatalog catalog;
        std::vector<lux::render::FeatureAttach> plan;
        lux::ecs::ComponentTypeCatalog components;
        std::unique_ptr<lux::runtime::ResidencyAssembly> residency;

        Fixture()
        {
            if (async.valid())
            {
                residency = std::make_unique<lux::runtime::ResidencyAssembly>(
                    control,
                    lux::render::RenderUploadClient{},
                    assets,
                    catalog,
                    async.client(),
                    async.runtime(),
                    lux::runtime::ResidencyAssembly::FailureSink{});
            }
        }

        ~Fixture()
        {
            if (residency)
            {
                const auto report =
                    lux::runtime::testing::detail::closeResidency(
                        *residency,
                        async.runtime());
                if (!report.clean())
                    std::abort();
                residency.reset();
            }
        }

        [[nodiscard]] lux::runtime::SceneRuntime::Dependencies dependencies()
        {
            return {
                assets,
                async.client(),
                async.runtime(),
                components,
                {}};
        }

        [[nodiscard]] lux::runtime::RenderSceneServices renderServices(
            const lux::runtime::RenderProfile& profile)
        {
            return {
                frame,
                control,
                {},
                catalog,
                plan,
                *residency,
                profile};
        }
    };

    const lux::runtime::RenderProfile& emptyProfile()
    {
        static constexpr lux::runtime::RenderProfile kProfile{};
        return kProfile;
    }
}

int main()
{
    {
        Fixture fixture;
        if (!check(fixture.async.valid() && fixture.residency != nullptr,
                   "local preflight fixture assembles"))
            return 1;
        lux::runtime::SceneRuntime::Config scene;
        scene.name = "missing-feature";
        scene.scene_asset_id = registerScene(
            fixture.assets,
            "81000000-0000-4000-8000-000000000001");
        auto services = fixture.renderServices(emptyProfile());
        lux::runtime::RenderSceneConfig render;
        render.target = lux::render::RenderTargetId{1u, 1u};
        render.install_rendering = [](
            lux::ecs::ScheduleBuilder&,
            const lux::scene::SceneDescription&,
            std::vector<std::unique_ptr<lux::ecs::RenderStage>>&,
            std::vector<std::string_view>& roots,
            lux::ecs::ResidencySubsystem&)
        {
            roots.push_back("org.lux.test.missing");
            return true;
        };
        auto runtime = lux::runtime::createRenderedSceneRuntime(
            fixture.dependencies(),
            std::move(scene),
            services,
            std::move(render));
        lux::render::OperationPacket<> packet;
        if (!check(!runtime, "unknown required Feature rejects the World") ||
            !check(
                fixture.control_channel->requests.tryPop(packet) ==
                    lux::cxx::EQueuePopResult::EMPTY,
                "local preflight failure performs zero remote operations"))
        {
            return 1;
        }
    }

    {
        Fixture fixture;
        if (!check(fixture.async.valid() && fixture.residency != nullptr,
                   "remote rollback fixture assembles"))
            return 1;
        fixture.catalog.add(
            lux::render::kLineListFeatureFactory,
            41u,
            {});
        fixture.plan.push_back(lux::render::FeatureAttach{
            "LineListTransient", {}, 41u, {}});

        bool protocol_ok = true;
        bool destroy_seen = false;
        std::thread server([&]
        {
            lux::render::OperationPacket<> packet;
            protocol_ok = popCommand(fixture.control_channel, packet) &&
                packet.has_command &&
                packet.command.type_id == lux::render::type_ids::CreateScene;
            if (!protocol_ok)
                return;
            protocol_ok = publishReply(
                fixture.control_channel,
                fixture.sync,
                lux::render::type_ids::ReplySceneCreated,
                packet.command.request_id,
                lux::render::SceneCreatedReply{
                    lux::render::RenderSceneId{7u, 3u}, {}});
            if (!protocol_ok ||
                !popCommand(fixture.control_channel, packet))
                return;
            protocol_ok = packet.has_command &&
                packet.command.type_id ==
                    lux::render::type_ids::SetActiveScene &&
                publishReply(
                    fixture.control_channel,
                    fixture.sync,
                    lux::render::type_ids::ReplyGenericOk,
                    packet.command.request_id,
                    lux::render::GenericOkReply{});
            if (!protocol_ok ||
                !popCommand(fixture.control_channel, packet))
                return;
            protocol_ok = packet.has_command &&
                packet.command.type_id == lux::render::type_ids::AddFeature &&
                publishReply(
                    fixture.control_channel,
                    fixture.sync,
                    lux::render::type_ids::ReplyFeatureAdded,
                    packet.command.request_id,
                    lux::render::FeatureAddedReply{});
            if (!protocol_ok ||
                !popCommand(fixture.control_channel, packet))
                return;
            destroy_seen = packet.has_command &&
                packet.command.type_id == lux::render::type_ids::DestroyScene;
        });

        lux::runtime::SceneRuntime::Config scene;
        scene.name = "remote-attach-failure";
        scene.scene_asset_id = registerScene(
            fixture.assets,
            "82000000-0000-4000-8000-000000000001");
        auto services = fixture.renderServices(emptyProfile());
        lux::runtime::RenderSceneConfig render;
        render.target = lux::render::RenderTargetId{1u, 1u};
        auto runtime = lux::runtime::createRenderedSceneRuntime(
            fixture.dependencies(),
            std::move(scene),
            services,
            std::move(render));
        server.join();
        if (!check(!runtime, "remote Feature refusal keeps World unpublished") ||
            !check(protocol_ok, "fake renderer observes the assembly protocol") ||
            !check(destroy_seen,
                   "remote assembly failure immediately rolls back its Scene"))
        {
            return 1;
        }
    }
    return 0;
}
