#pragma once

#include <lux/engine/render/data/Encoding.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <lux/engine/render/features/Assembly.hpp>

namespace fixture
{
    using namespace lux::render;
    inline constexpr EncodedDataLimits limits{4096, 4096};
    inline std::atomic<unsigned> descriptions{0}, builds{0};
    enum class ERole { GEOMETRY, LIGHTING, ALTERNATE, TONEMAP, HISTORY, CULL, MATERIAL, MESH };

    template <ERole R> constexpr FeatureName type() noexcept
    {
        if constexpr (R == ERole::GEOMETRY) return FeatureName("test.geometry");
        if constexpr (R == ERole::LIGHTING) return FeatureName("test.lighting");
        if constexpr (R == ERole::ALTERNATE) return FeatureName("test.alternate");
        if constexpr (R == ERole::TONEMAP) return FeatureName("test.tonemap");
        if constexpr (R == ERole::HISTORY) return FeatureName("test.history");
        if constexpr (R == ERole::CULL) return FeatureName("test.cull");
        if constexpr (R == ERole::MATERIAL) return FeatureName("test.material");
        return FeatureName("test.mesh");
    }

    inline ResourceClaim image(std::string key, EResourceClaim participation)
    {
        return {std::move(key), participation, GraphImageDesc{64, 64}};
    }

    inline ResourceClaim material(EResourceClaim participation)
    {
        return {"material", participation, GraphBufferDesc{256}, EGraphOrigin::IMPORTED, {{}},
            StorageRequirement{StorageName("test.MaterialStorage"), {1, 0}, EResourceScope::GLOBAL}};
    }

    inline ResourceClaim history(std::string key, EResourceClaim participation, bool previous)
    {
        auto c = image(std::move(key), participation);
        c.origin = previous ? EGraphOrigin::HISTORY_PREVIOUS : EGraphOrigin::HISTORY_CURRENT;
        if (previous) c.initial_ranges.push_back({});
        c.storage = StorageRequirement{StorageName("test.HzbStorage"), {1, 0}, EResourceScope::VIEW};
        return c;
    }

    template <ERole R>
    RenderResult<void> describe(const void*, std::string_view, std::vector<ResourceClaim>& out) noexcept
    {
        ++descriptions;
        using enum EResourceClaim;
        if constexpr (R == ERole::GEOMETRY) out = {image("gbuffer", PROVIDE)};
        if constexpr (R == ERole::LIGHTING || R == ERole::ALTERNATE)
            out = {image("gbuffer", REQUIRE), image("hdr", PROVIDE)};
        if constexpr (R == ERole::TONEMAP) out = {image("hdr", REQUIRE), image("color", PROVIDE)};
        if constexpr (R == ERole::HISTORY) out = {history("hzb.previous", PROVIDE, true)};
        if constexpr (R == ERole::CULL)
            out = {history("hzb.previous", REQUIRE, true), history("hzb.current", PROVIDE, false)};
        if constexpr (R == ERole::MATERIAL) out = {material(PROVIDE)};
        if constexpr (R == ERole::MESH) out = {material(REQUIRE), image("color", PROVIDE)};
        return {};
    }

    inline RenderResult<void> pass(
        RenderGraphBuilder& builder, std::string name, std::span<const GraphVersion> inputs,
        GraphVersion output, EGraphPassKind kind = EGraphPassKind::COMPUTE
    ) noexcept
    {
        GraphPass p{std::move(name), kind};
        for (auto input : inputs) p.uses.push_back({input, {}, EGraphAccess::READ});
        p.uses.push_back({output, {}, EGraphAccess::WRITE});
        auto added = builder.addPass(std::move(p));
        if (!added) return lux::cxx::unexpected(added.error());
        return {};
    }

    template <ERole R>
    RenderResult<void> build(
        const void*, std::string_view instance, std::span<const ResolvedResource> resolved,
        RenderGraphBuilder& builder
    ) noexcept
    {
        ++builds;
        const auto key = [&](std::string_view local) { return featureKey(type<R>(), instance, local); };
        const auto port = [&](std::string_view name) { return findResource(resolved, name)->version; };
        if constexpr (R == ERole::GEOMETRY)
        {
            auto scratch = builder.declareResource({key("scratch"), GraphImageDesc{64, 64}});
            if (!scratch) return lux::cxx::unexpected(scratch.error());
            if (auto r = pass(builder, key("prepare"), {}, {*scratch, 1}); !r) return r;
            const std::array inputs{GraphVersion{*scratch, 1}};
            return pass(builder, key("gbuffer"), inputs, port("gbuffer"), EGraphPassKind::GRAPHICS);
        }
        if constexpr (R == ERole::LIGHTING || R == ERole::ALTERNATE)
        {
            const std::array inputs{port("gbuffer")};
            return pass(builder, key("lighting"), inputs, port("hdr"));
        }
        if constexpr (R == ERole::TONEMAP)
        {
            const std::array inputs{port("hdr")};
            if (auto r = pass(builder, key("tonemap"), inputs, port("color")); !r) return r;
            // Multiple exports, including an independently produced private output.
            auto side = builder.declareResource({key("side"), GraphBufferDesc{16}});
            if (!side) return lux::cxx::unexpected(side.error());
            if (auto r = pass(builder, key("side"), {}, {*side, 1}); !r) return r;
            if (auto r = builder.exportOutput({{*side, 1}}); !r) return r;
            auto dead = builder.declareResource({key("dead"), GraphBufferDesc{16}});
            if (!dead) return lux::cxx::unexpected(dead.error());
            if (auto r = pass(builder, key("dead"), {}, {*dead, 1}); !r) return r;
            return builder.exportOutput({port("color")});
        }
        if constexpr (R == ERole::CULL)
        {
            const std::array inputs{port("hzb.previous")};
            if (auto r = pass(builder, key("cull"), inputs, port("hzb.current")); !r) return r;
            return builder.exportOutput({port("hzb.current")});
        }
        if constexpr (R == ERole::MESH)
        {
            const std::array inputs{port("material")};
            if (auto r = pass(builder, key("draw"), inputs, port("color"), EGraphPassKind::GRAPHICS); !r) return r;
            return builder.exportOutput({port("color")});
        }
        return {};
    }

    template <ERole R> const FeatureDescriptor& definition() noexcept
    {
        static const FeatureDescriptor value{type<R>(), {}};

        return value;
    }

    template <ERole R> const FeatureGraphOps& graph() noexcept
    {
        static const FeatureGraphOps value{&describe<R>, (R == ERole::HISTORY || R == ERole::MATERIAL) ? nullptr : &build<R>};
        return value;
    }

    inline RenderResult<FeatureAssembly> scenario(unsigned which) noexcept
    {
        if (which == 0)
        {
            const FeatureInstanceView chosen[]{
                {&definition<ERole::TONEMAP>(), &graph<ERole::TONEMAP>(), nullptr, "main"},
                {&definition<ERole::LIGHTING>(), &graph<ERole::LIGHTING>(), nullptr, "main"},
                {&definition<ERole::GEOMETRY>(), &graph<ERole::GEOMETRY>(), nullptr, "main"}
            };
            return assembleFeatures(chosen, limits.max_encoded_bytes);
        }
        const FeatureInstanceView chosen[]{
            {which == 1 ? &definition<ERole::HISTORY>() : &definition<ERole::MATERIAL>(),
                which == 1 ? &graph<ERole::HISTORY>() : &graph<ERole::MATERIAL>(), nullptr, "main"},
            {which == 1 ? &definition<ERole::CULL>() : &definition<ERole::MESH>(),
                which == 1 ? &graph<ERole::CULL>() : &graph<ERole::MESH>(), nullptr, "main"}
        };
        return assembleFeatures(chosen, limits.max_encoded_bytes);
    }

    struct Config final
    {
        std::uint32_t first = 0, count = 1, total = 1, chain = 0;
    };
}

namespace lux::render
{
    template <> struct DataSchema<fixture::Config> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr std::uint64_t boolean[]{0, 1};
            static constexpr DataField fields[]{
                {"first", cxx::EScalarKind::U32}, {"count", cxx::EScalarKind::U32},
                {"total", cxx::EScalarKind::U32},
                {"chain", cxx::EScalarKind::U32, 1, EDataFieldKind::SCALAR, 1, boolean}
            };
            static constexpr DataSchemaDescriptor value{
                DataTypeIdView("test.feature.config"), {1, 0}, {1, 0}, fields, 16, 4
            };
            return value;
        }

        static RenderResult<std::size_t> encodedSize(const fixture::Config&) noexcept { return 16; }

        static RenderResult<void> encode(const fixture::Config& c, cxx::BinarySpanWriter& w) noexcept
        {
            w.writeUnsigned(c.first);
            w.writeUnsigned(c.count);
            w.writeUnsigned(c.total);
            w.writeUnsigned(c.chain);
            return {};
        }

        static RenderResult<fixture::Config> decode(cxx::BinaryReader& r, RetainableEncodedDataView) noexcept
        {
            fixture::Config c;
            r.readUnsigned(c.first);
            r.readUnsigned(c.count);
            r.readUnsigned(c.total);
            r.readUnsigned(c.chain);
            return c;
        }
    };
}
