#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/features/material/MaterialOperation.hpp>
#include <lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace
{
    struct BulkValue final
    {
        std::uint64_t first{};
        std::uint64_t second{};
    };

    static_assert(lux::render::FrameBlobPayload<BulkValue>);
}

int main()
{
    using namespace lux::render;

    auto channel = RenderProgramChannel<>::create(1U);
    auto sync = std::make_shared<RenderChannelSync>();
    RenderProgramSession session(channel, sync);

    const lux::asset::AssetId asset_id{std::array<std::uint8_t, 16>{1U, 2U, 3U, 4U}};
    UploadMeshPayload mesh_upload{};
    mesh_upload.asset_id = asset_id;
    UploadGraphMaterialPayload material_upload{};
    material_upload.asset_id = asset_id;
    assert(mesh_upload.asset_id == asset_id);
    assert(material_upload.asset_id == asset_id);

    RenderProgram<> source;
    RenderProgramBuilder<> builder(source);
    builder.begin(ProgramMemoryHints{.command_capacity = 4U, .payload_capacity = 256U});
    source.kind = ERenderProgramKind::StateUpdate;
    auto values = builder.appendBulk<BulkValue>(71U, 2U);
    assert(values.size() == 2U);
    assert(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(BulkValue) == 0U);
    values[0] = {1U, 2U};
    values[1] = {3U, 4U};
    const auto payload_capacity = source.payload.capacity();

    assert(session.trySubmitPrepared(source));
    assert(source.commands.empty());
    assert(source.payload.empty());
    assert(source.attachments.empty());
    assert(!session.hasPendingSubmit());
    assert(channel->requests.tryAcquireRead());
    const auto& submitted = channel->requests.currentRead();
    assert(submitted.kind == ERenderProgramKind::StateUpdate);
    assert(submitted.commands.size() == 1U);
    assert(submitted.payload.capacity() >= payload_capacity);
    const auto* submitted_values = reinterpret_cast<const BulkValue*>(submitted.payload.data());
    assert(submitted_values[0].first == 1U && submitted_values[1].second == 4U);

    RenderProgram<> blocked;
    RenderProgramBuilder<> blocked_builder(blocked);
    blocked_builder.begin();
    blocked.kind = ERenderProgramKind::StateUpdate;
    blocked_builder.push(opcodes::CommandOp, 2U, BulkValue{9U, 10U});
    const auto blocked_size = blocked.payload.size();
    assert(session.beginFrame());
    assert(!session.trySubmitPrepared(blocked));
    assert(blocked.payload.size() == blocked_size);
    assert(blocked.commands.size() == 1U);
    assert(session.trySubmitFrame());
    return 0;
}
