#include "Attachments.pass.hpp"
#include "NonShader.pass.hpp"
#include "Optional.pass.hpp"
#include "Stages.pass.hpp"
#include "Storage.pass.hpp"
#include "Tonemap.pass.hpp"
#include <bit>
#include <iostream>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/graph/DefinitionAccess.hpp>
#include <memory>

using namespace lux::render;
using lux::rdesc::EPassFieldRole;
using lux::rdesc::ETextureFormat;
static int failures{};

static void check(bool condition, const char* label)
{
    if (!condition)
    {
        std::cerr << label << '\n';
        ++failures;
    }
}

static RenderResult<GraphPassId> add(RenderGraphBuilder& builder, GraphTexture input, GraphTexture output)
{
    Tonemap params;
    params.input.texture = input;
    params.output.texture = output;
    params.linear.sampler = GraphSampler{1};
    return builder.addPass("tonemap", {"tonemap", "default"}, EPassKind::GRAPHICS, EExecutionScope::VIEW, params);
}

static void rejected(const RenderResult<GraphPassId>& result, const char* label)
{
    check(!result && result.error().type == kGraphInvalidUse, label);
}

static void scopes(bool different_kind)
{
    RenderGraphBuilder a, b;
    const auto foreign = a.texture({});
    GraphTexture local;
    if (different_kind)
    {
        const auto buffer = b.buffer({64, 4});
        check(foreign.value() == buffer.value(), "cross-kind collision exercises same declaration position");
    }
    else
    {
        local = b.texture({});
        check(foreign.value() == local.value() && foreign != local, "same numeric position, distinct typed identity");
    }
    const auto output = b.texture({});
    rejected(add(b, foreign, output), "foreign texture rejected before Definition loses authoring scope");
    if (!different_kind)
    {
        check(bool(add(b, local, output)), "same Builder accepted after rejected candidate");
    }
    Optional optional;
    optional.group.input.fallback = foreign;
    optional.output.texture = output;
    optional.linear.sampler = GraphSampler{1};
    const auto result =
        b.addPass("optional", {"optional", "default"}, EPassKind::GRAPHICS, EExecutionScope::VIEW, optional);
    rejected(result, "optional fallback must also belong to the Builder");
    check(bool(std::move(b).finish()), "rejected candidate leaves same Builder Definition valid");

    RenderGraphBuilder c, d;
    const auto foreign_buffer = c.buffer({64, 4});
    if (different_kind)
    {
        (void)d.texture({});
    }
    else
    {
        (void)d.buffer({64, 4});
    }
    Storage storage;
    storage.values.buffer = foreign_buffer;
    storage.output.texture = d.texture({});
    rejected(
        d.addPass("storage", {"storage", "default"}, EPassKind::COMPUTE, EExecutionScope::VIEW, storage),
        "foreign buffer rejected for both colliding backing kinds"
    );
}

static void lifecycle()
{
    RenderGraphBuilder first;
    const auto input = first.texture({});
    const auto output = first.texture({});
    RenderGraphBuilder moved(std::move(first));
    (void)first.texture({});
    rejected(add(first, input, first.texture({})), "moved-from Builder has a fresh independent scope");
    RenderGraphBuilder assigned;
    const auto replaced = assigned.texture({});
    assigned = std::move(moved);
    rejected(add(assigned, replaced, output), "move assignment invalidates destination's old references");
    check(bool(add(assigned, input, output)), "move transfers resource scope with declarations");
    auto definition = std::move(assigned).finish();
    check(bool(definition), "moved Builder finishes");
    const auto fresh_input = assigned.texture({});
    const auto fresh_output = assigned.texture({});
    rejected(add(assigned, input, fresh_output), "finish/reuse rejects stale scope with same slot");
    check(bool(add(assigned, fresh_input, fresh_output)), "reused Builder accepts fresh references");
    const auto equivalent = std::move(assigned).finish();
    check(definition && equivalent && *definition == *equivalent, "authoring scope does not change topology identity");

    // Reuse the exact Builder address to prove this is not a pointer-only scope check.
    alignas(RenderGraphBuilder) std::byte storage[sizeof(RenderGraphBuilder)];
    auto* old = std::construct_at(reinterpret_cast<RenderGraphBuilder*>(storage));
    const auto expired = old->texture({});
    std::destroy_at(old);
    auto* replacement = std::construct_at(reinterpret_cast<RenderGraphBuilder*>(storage));
    (void)replacement->texture({});
    rejected(add(*replacement, expired, replacement->texture({})), "same address after destruction rejects old handle");
    std::destroy_at(replacement);
}

static void forged()
{
    RenderGraphBuilder builder;
    const auto buffer = builder.buffer({64, 4});
    const auto image = builder.texture({});
    rejected(add(builder, std::bit_cast<GraphTexture>(buffer), image), "same-scope forged Texture kind rejected");
    Storage forged_params;
    forged_params.values.buffer = std::bit_cast<GraphBuffer>(image);
    forged_params.output.texture = image;
    rejected(
        builder.addPass(
            "forged.storage",
            {"storage", "default"},
            EPassKind::COMPUTE,
            EExecutionScope::VIEW,
            forged_params
        ),
        "same-scope forged Buffer kind rejected"
    );
    rejected(add(builder, GraphTexture{buffer.value()}, image), "forged Texture from Buffer slot rejected");
    Storage params;
    params.values.buffer = GraphBuffer{image.value()};
    params.output.texture = image;
    rejected(
        builder.addPass("storage", {"storage", "default"}, EPassKind::COMPUTE, EExecutionScope::VIEW, params),
        "forged Buffer from Texture slot rejected"
    );
    rejected(add(builder, GraphTexture{image.value()}, image), "numeric same-kind reconstruction is also unscoped");
}

static void roleKinds()
{
    // Every resource role is validated independently of capture. Even a tampered binding
    // whose declared kind agrees with the wrong backing cannot bypass its semantic role.
    for (const auto role :
         {EPassFieldRole::SAMPLED_READ,
          EPassFieldRole::STORAGE_READ,
          EPassFieldRole::STORAGE_WRITE,
          EPassFieldRole::STORAGE_READ_WRITE,
          EPassFieldRole::COLOR_ATTACHMENT,
          EPassFieldRole::DEPTH_STENCIL,
          EPassFieldRole::RESOLVE,
          EPassFieldRole::INPUT_ATTACHMENT,
          EPassFieldRole::UNIFORM_READ,
          EPassFieldRole::READ_ONLY_STORAGE,
          EPassFieldRole::READ_WRITE_STORAGE,
          EPassFieldRole::VERTEX,
          EPassFieldRole::INDEX,
          EPassFieldRole::INDIRECT})
    {
        const bool buffer_role = role == EPassFieldRole::UNIFORM_READ || role == EPassFieldRole::READ_ONLY_STORAGE ||
                                 role == EPassFieldRole::READ_WRITE_STORAGE || role == EPassFieldRole::VERTEX ||
                                 role == EPassFieldRole::INDEX || role == EPassFieldRole::INDIRECT;
        const auto correct = buffer_role ? EGraphResourceKind::BUFFER : EGraphResourceKind::IMAGE;
        const auto wrong = buffer_role ? EGraphResourceKind::IMAGE : EGraphResourceKind::BUFFER;
        for (const auto declared : {correct, wrong})
        {
            GraphResource resource;
            resource.kind = wrong;
            GraphFieldBinding field;
            field.resource = GraphResourceId{1};
            field.resource_kind = declared;
            field.role = role;
            GraphPass pass;
            pass.bindings.push_back(field);
            const auto result = detail::DefinitionAccess::create({resource}, {pass});
            check(!result && result.error().type == kGraphInvalidUse, "Definition rejects semantic role/kind mismatch");
        }
    }
    for (const auto role : {EPassFieldRole::TRANSFER_SOURCE, EPassFieldRole::TRANSFER_DESTINATION})
    {
        GraphResource resource;
        GraphFieldBinding field;
        field.resource = GraphResourceId{1};
        field.resource_kind = EGraphResourceKind::IMAGE;
        field.role = role;
        GraphPass pass;
        pass.bindings.push_back(field);
        check(!detail::DefinitionAccess::create({resource}, {pass}), "transfer wrapper kind still checked");
    }
    RenderGraphBuilder builder;
    Storage params;
    params.values.buffer = builder.buffer({64, 4});
    params.output.texture = builder.texture({});
    const auto captured = PassSchema<Storage>::capture(params);
    check(captured.bindings[0].resource_kind == EGraphResourceKind::BUFFER, "generated SSBO capture records Buffer");
    check(captured.bindings[1].resource_kind == EGraphResourceKind::IMAGE, "generated storage image records Texture");
    check(
        bool(builder.addPass("storage", {"storage", "default"}, EPassKind::COMPUTE, EExecutionScope::VIEW, params)),
        "valid Shader resource kinds accepted"
    );
    check(bool(std::move(builder).finish()), "valid Shader kinds finish");
}

static RenderResult<RenderGraphDefinition> depth(ETextureFormat format, DepthStencilAttachment value)
{
    RenderGraphBuilder builder;
    TextureDesc desc;
    desc.samples = 4;
    Attachments params;
    params.color.texture = builder.texture(desc);
    params.resolved.texture = builder.texture({});
    desc.format = format;
    value.texture = builder.texture(desc);
    params.depth = value;
    const auto result =
        builder.addPass("depth", {"depth", "default"}, EPassKind::GRAPHICS, EExecutionScope::VIEW, params);
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    return std::move(builder).finish();
}

static void depthOperations()
{
    for (const auto format : {ETextureFormat::D16_UNORM, ETextureFormat::D32_SFLOAT})
    {
        for (const auto op : {ELoadOp::LOAD, ELoadOp::CLEAR})
        {
            DepthStencilAttachment value;
            value.stencil_load = op;
            const auto result = depth(format, value);
            check(!result && result.error().type == kGraphInvalidUse, "depth-only format rejects stencil load/clear");
        }
        DepthStencilAttachment value;
        value.stencil_store = EStoreOp::STORE;
        check(!depth(format, value), "depth-only format rejects stencil store");
        value.stencil_store = EStoreOp::DISCARD;
        check(bool(depth(format, value)), "depth-only default operations stay valid");
    }
    for (const auto format :
         {ETextureFormat::D16_UNORM_S8_UINT, ETextureFormat::D24_UNORM_S8_UINT, ETextureFormat::D32_SFLOAT_S8_UINT})
    {
        DepthStencilAttachment value;
        value.stencil_load = ELoadOp::CLEAR;
        check(!depth(format, value), "stencil clear requires declared stencil aspect even on DS format");
        value.stencil_load = ELoadOp::LOAD;
        check(!depth(format, value), "stencil load requires declared stencil aspect");
        value.stencil_load = ELoadOp::DISCARD;
        value.stencil_store = EStoreOp::STORE;
        check(!depth(format, value), "stencil store requires declared stencil aspect");
        value.range.aspect = EAspect::DEPTH_STENCIL;
        value.stencil_load = ELoadOp::CLEAR;
        auto combined = depth(format, value);
        check(bool(combined), "combined DS load/store valid");
        if (combined)
        {
            const auto& pass = combined->passes()[0];
            check(pass.bindings[2].image_range == pass.uses[2].image_range, "generated DS range equals captured use");
        }
        value.range.aspect = EAspect::STENCIL;
        check(!depth(format, value), "stencil-only view rejects depth clear/store");
        value.load = ELoadOp::DISCARD;
        value.store = EStoreOp::DISCARD;
        value.stencil_load = ELoadOp::LOAD;
        const auto stencil = depth(format, value);
        check(bool(stencil), "stencil-only DS view with no depth operations valid");
        if (stencil)
        {
            check(
                stencil->passes()[0].uses[2].access == EGraphAccess::READ_WRITE,
                "stencil load remains read/write use"
            );
        }
    }
}

static void aspects()
{
    for (const auto format : {ETextureFormat::D32_SFLOAT, ETextureFormat::RGBA8_UNORM})
    {
        for (const auto aspect :
             {EAspect::STENCIL,
              EAspect::DEPTH_STENCIL,
              static_cast<EAspect>(0),
              static_cast<EAspect>(8),
              static_cast<EAspect>(7)})
        {
            DepthStencilAttachment value;
            value.range.aspect = aspect;
            const auto result = depth(format, value);
            check(!result && result.error().type == kGraphInvalidUse, "format/aspect mismatch rejected");
        }
    }
    DepthStencilAttachment value;
    value.range.aspect = EAspect::COLOR;
    check(!depth(ETextureFormat::D24_UNORM_S8_UINT, value), "DS target rejects color aspect");
    check(!depth(ETextureFormat::RGBA8_UNORM, value), "color target cannot be depth/stencil attachment");
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        return 2;
    }
    const std::string_view mode = argv[1];
    if (mode == "same_kind")
    {
        scopes(false);
    }
    else if (mode == "cross_kind")
    {
        scopes(true);
    }
    else if (mode == "lifecycle")
    {
        lifecycle();
    }
    else if (mode == "forged")
    {
        forged();
    }
    else if (mode == "roles")
    {
        roleKinds();
    }
    else if (mode == "depth_ops")
    {
        depthOperations();
    }
    else if (mode == "aspects")
    {
        aspects();
    }
    else
    {
        return 2;
    }
    std::cout << mode << " failures=" << failures << '\n';
    return failures != 0;
}
