#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "SceneFixture.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <thread>

using namespace lux::render;
using namespace std::chrono_literals;
static void check(bool value) { if (!value) std::abort(); }
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

struct AssetConfig final { ExternalDataRef ref; };
namespace lux::render
{
    template <> struct DataSchema<AssetConfig>
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"asset", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}};
            static const DataSchemaDescriptor schema{DataTypeIdView("test.asset.config"), {1, 0}, {1, 0}, fields, 20, 4};
            return schema;
        }
        static RenderResult<std::size_t> wireSize(const AssetConfig&) noexcept { return 20; }
        static RenderResult<void> encode(const AssetConfig& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.ref.attachment_index);
            out.writeUnsigned(value.ref.offset);
            out.writeUnsigned(value.ref.size);
            return {};
        }
        static RenderResult<AssetConfig> decode(cxx::BinaryReader& in, WireView) noexcept
        {
            AssetConfig result;
            in.readUnsigned(result.ref.attachment_index);
            in.readUnsigned(result.ref.offset);
            in.readUnsigned(result.ref.size);
            return result;
        }
    };
}

static std::latch* entered = nullptr;
static std::latch* resume = nullptr;
static std::atomic<const std::byte*> observed{};

static RenderResult<void> describeAsset(const WireView* wire, std::string_view, std::vector<ResourceClaim>&) noexcept
{
    auto config = decodeWire<AssetConfig>(*wire, {1024, 1024});
    if (!config) return lux::cxx::unexpected(config.error());
    auto asset = resolveExternal(wire->attachments, config->ref, 1024);
    if (!asset) return lux::cxx::unexpected(asset.error());
    check(asset->view().front() == std::byte{67} && asset->view().back() == std::byte{67});
    observed.store(asset->data());
    if (entered) { entered->count_down(); resume->wait(); }
    return {};
}

static const FeatureDefinition asset_definition{
    FeatureTypeIdView("test.asset"), {1, 0}, &DataSchema<AssetConfig>::describe(), describeAsset, nullptr
};

static RenderResult<std::vector<PassProgram>> assetPrograms(const WireView*, std::string_view) noexcept { return {}; }

static RenderResult<std::vector<PassProgram>> programs(const WireView* wire, std::string_view name) noexcept
{
    auto config = decodeWire<native_fixture::Config>(*wire, native_fixture::limits);
    if (!config) return lux::cxx::unexpected(config.error());
    return native_fixture::programs(*config, name);
}

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = take(receiveExpected(client, id));
        if (result) return std::move(*result);
        std::this_thread::sleep_for(1ms);
    }
    std::abort();
}

static RenderCommand effect(SessionIdentity identity)
{
    RenderCommand result;
    result.operation = ERenderCommand::CREATE_EFFECT;
    result.session = identity;
    native_fixture::Config config;
    result.description = native_fixture::target(config);
    result.features.push_back({std::string(native_fixture::type.name()), "main", {1, 0},
        &DataSchema<native_fixture::Config>::describe(), take(encodeWire(config, {}, native_fixture::limits))});
    return result;
}

int main()
{
    auto server = take(RenderServer::create({"runtime.life", true}));
    check(bool(server->registerFeature({&native_fixture::definition, programs})));
    check(bool(server->registerFeature({&asset_definition, assetPrograms})));
    auto main = take(RenderClient::create("runtime.life", {1024 * 1024, 64}));
    auto upload = take(RenderClient::create("runtime.life", {2 * 1024 * 1024, 64}));
    std::thread thread([&] { check(bool(server->run())); });
    auto ready = wait(*main, take(main->submit({})));
    const auto identity = ready.session;
    ready = {};
    auto ready_upload = wait(*upload, take(upload->submit({})));
    check(ready_upload.session == identity);
    ready_upload = {};
    std::atomic<unsigned> destroyed{0};
    auto source = std::shared_ptr<const std::vector<std::byte>>(
        new std::vector<std::byte>(1024 * 1024, std::byte{67}), [&](auto* bytes) { delete bytes; ++destroyed; });
    const auto* pointer = source->data() + 257;
    auto command = effect(identity);
    command.attachments.push_back(take(RenderPayload::fromVector(source, 257, 64)));
    std::array attachments{command.attachments[0].bytes()};
    auto config = take(encodeWire(AssetConfig{{0, 0, 64}}, attachments, {1024, 1024}));
    command.features.push_back({"test.asset", "pin", {1, 0}, &DataSchema<AssetConfig>::describe(), config});
    check(!main->submit(command)); // 64 visible bytes must charge the entire 1 MiB backing
    check(command.attachments[0].bytes().data() == pointer);
    auto id = take(upload->submit(command));
    auto reply = wait(*upload, id);
    check(reply.header.outcome == EOperationOutcome::SUCCESS && observed.load() == pointer);
    check(upload->retainedBytes() >= 1024 * 1024);
    check(!upload->submit(command)); // held result retains the input lease
    reply = {};
    check(upload->retainedBytes() == 0);
    id = take(upload->submit(command));
    check(bool(upload->abandon(id)));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (upload->retainedBytes() && std::chrono::steady_clock::now() < deadline)
    {
        RenderCompletion discarded;
        check(take(upload->tryReceive(discarded)) == ERenderReceive::EMPTY);
        std::this_thread::sleep_for(1ms);
    }
    check(upload->retainedBytes() == 0);

    std::latch in_callback(1), continue_callback(1);
    entered = &in_callback;
    resume = &continue_callback;
    id = take(upload->submit(command));
    in_callback.wait();
    command.attachments.clear();
    source.reset();
    attachments = {};
    upload.reset();
    check(destroyed == 0 && observed.load() == pointer);
    server->requestStop();
    continue_callback.count_down();
    thread.join();
    check(destroyed == 1);
    check(server->stats().validation_errors == 0);
    entered = resume = nullptr;
    server.reset();

    auto replacement = take(RenderServer::create({"runtime.life", true}));
    auto fresh = take(RenderClient::create("runtime.life"));
    std::thread restart([&] { check(bool(replacement->run())); });
    auto current = wait(*fresh, take(fresh->submit({})));
    check(current.session != identity);
    RenderCommand old;
    old.operation = ERenderCommand::CREATE_SCENE;
    old.session = identity;
    check(wait(*fresh, take(fresh->submit(old))).header.outcome == EOperationOutcome::FAILED);
    replacement->requestStop();
    restart.join();
    std::printf("M3 lifetime: full backing budget, pointer identity, abandon reclamation, close during callback, epoch OK\n");
}
