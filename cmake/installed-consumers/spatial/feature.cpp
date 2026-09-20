#include <cassert>
#include <cstdio>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <probe/generated/Probe.ops.hpp>

int main()
{
    using namespace lux::render;
    FeatureCatalog catalog;
    FeatureFactory registration;
    registration.name = "ExternalProbe";
    registration.descriptor = kExternalProbeDescriptor;
    const std::array<TypeId, 1> dynamic_ids{701};
    catalog.add(registration, 31, dynamic_ids);
    const auto ops = catalog.ops<ExternalProbeOperationIds>("ExternalProbe");
    assert(ops.valid() && catalog.typeId("ExternalProbe") == 31);
    assert(catalog.nameOfType(featureId("test.external.probe.v1")) == "ExternalProbe");

    auto channel = RenderProgramChannel<>::create(2);
    auto sync = std::make_shared<RenderChannelSync>();
    RenderProgramSession session(channel, sync);
    assert(session.beginFrame());
    ExternalProbeProxy proxy(session, ops);
    const std::array values{probe::Value{1, 2.5}, probe::Value{2, 7.5}};
    proxy.set(values);
    assert(session.trySubmitFrame());
    assert(channel->requests.tryAcquireRead());
    const auto &packet = channel->requests.currentRead();
    assert(packet.commands.size() == 1);
    const auto &command = packet.commands.front();
    assert(command.type_id == dynamic_ids.front() && command.payload_size == sizeof(values));
    const auto *received = reinterpret_cast<const probe::Value *>(packet.payload.data() + command.payload_offset);
    assert(received[0].sequence == 1 && received[1].sequence == 2 && received[0].amount + received[1].amount == 10);
    session.requestStop();
    std::puts("PASS external generated Feature DLL: dynamic catalog ID and actual Program channel; no builtin "
              "Feature/backend imports");
}
