#include "Scenarios.hpp"

#include <barrier>
#include <cstdio>
#include <thread>

using namespace lux::render;
namespace cxx = lux::cxx;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

struct CountedConfig final { std::shared_ptr<const fixture::Config> value; };
namespace lux::render
{
    template <> struct ConfigValueTraits<CountedConfig>
    {
        static RenderResult<std::size_t> retainedBytes(const CountedConfig&) noexcept
        {
            return sizeof(CountedConfig) + sizeof(fixture::Config);
        }
    };
}

static RenderResult<void> emptyDescribe(const void*, std::string_view, ResourceClaims&) noexcept
{
    return {};
}

static int composition()
{
    using enum fixture::ERole;
    const std::array types{&fixture::definition<GEOMETRY>(), &fixture::definition<LIGHTING>(),
        &fixture::definition<TONEMAP>()};
    const std::array graphs{&fixture::graph<GEOMETRY>(), &fixture::graph<LIGHTING>(), &fixture::graph<TONEMAP>()};
    std::array<unsigned, 3> order{0, 1, 2};
    auto reference = fixture::scenario(0);
    CHECK(reference);
    CHECK(reference->definition.passes().size() == 6);
    CHECK(reference->analysis.execution_order.size() == 5);
    CHECK(reference->definition.outputs().size() == 2 && reference->external_bindings.empty());
    const std::vector<GraphPassId> expected_order{{0}, {1}, {2}, {3}, {4}};
    const std::vector<GraphDependency> expected_edges{{{0}, {1}}, {{1}, {2}}, {{2}, {3}}};
    CHECK(reference->analysis.execution_order == expected_order);
    CHECK(reference->analysis.dependencies == expected_edges);
    CHECK(std::ranges::any_of(reference->analysis.lifetimes, [](const GraphLifetime& l) {
        return l.version == GraphVersion{{1}, 1} && l.first == 1 && l.last == 2;
    }));
    do
    {
        std::vector<FeatureInstanceView> selected;
        for (auto i : order) selected.push_back({types[i], graphs[i], nullptr, "main"});
        auto result = assembleFeatures(selected, fixture::limits.max_encoded_bytes);
        CHECK(result && result->analysis == reference->analysis);
        for (std::size_t i = 0; i < result->definition.resources().size(); ++i)
            CHECK(result->definition.resources()[i].key == reference->definition.resources()[i].key);
        for (std::size_t i = 0; i < result->definition.passes().size(); ++i)
            CHECK(result->definition.passes()[i].key == reference->definition.passes()[i].key);
    } while (std::next_permutation(order.begin(), order.end()));
    const FeatureInstanceView alternate[]{
        {types[0], graphs[0], nullptr, "main"}, {&fixture::definition<ALTERNATE>(), &fixture::graph<ALTERNATE>(), nullptr, "main"},
        {types[2], graphs[2], nullptr, "main"}
    };
    auto alt = assembleFeatures(alternate, fixture::limits.max_encoded_bytes);
    CHECK(alt && alt->analysis.execution_order.size() == 5);
    const FeatureInstanceView conflict[]{
        alternate[0], alternate[1], alternate[2], {types[1], graphs[1], nullptr, "main"}
    };
    auto duplicate = assembleFeatures(conflict, fixture::limits.max_encoded_bytes);
    CHECK(!duplicate && duplicate.error().type == kFeatureDuplicate);
    auto hzb = fixture::scenario(1);
    CHECK(hzb && hzb->external_bindings.size() == 2 && hzb->analysis.execution_order.size() == 1);
    CHECK(hzb->external_bindings.view.size() == 2 && hzb->external_bindings.global.empty() &&
        hzb->external_bindings.scene.empty() && hzb->external_bindings.attachment.empty());
    const auto& hr = hzb->definition.resources();
    CHECK(hr.size() == 2 && hr[0].origin == EGraphOrigin::HISTORY_CURRENT);
    CHECK(hr[1].origin == EGraphOrigin::HISTORY_PREVIOUS);
    CHECK(hzb->definition.passes()[0].uses[0].version.number == 0);
    CHECK(hzb->definition.passes()[0].uses[0].version.resource !=
        hzb->definition.passes()[0].uses[1].version.resource);
    auto material = fixture::scenario(2);
    CHECK(material && material->external_bindings.size() == 1);
    CHECK(material->definition.resources().size() == 2 && material->definition.passes().size() == 1);
    CHECK(material->external_bindings.global[0].storage.type.name() == "test.MaterialStorage");
    CHECK(material->definition.passes()[0].uses[0].version.number == 0);
    return 0;
}

static int identity()
{
    auto empty = assembleFeatures({}, {});
    CHECK(empty && empty->analysis.execution_order.empty());
    FeatureDescriptor d{FeatureName("test.type"), {}};
    FeatureGraphOps d_graph{emptyDescribe, nullptr};
    FeatureInstanceView s{&d, &d_graph, nullptr, "one"};
    CHECK(assembleFeatures({&s, 1}, {}));
    FeatureInstanceView missing{};
    CHECK(!assembleFeatures({&missing, 1}, {}));
    s.instance_key = "";
    CHECK(!assembleFeatures({&s, 1}, {}));
    s.instance_key = "one";
    auto saved = d;
    auto saved_graph = d_graph;
    d.type = {};
    CHECK(!assembleFeatures({&s, 1}, {}));
    d = saved;
    d_graph = saved_graph;
    d.config = cppType<std::uint32_t>();
    CHECK(!assembleFeatures({&s, 1}, {}));
    d = saved;
    d_graph = saved_graph;
    d_graph.describe = nullptr;
    CHECK(!assembleFeatures({&s, 1}, {}));
    d = saved;
    d_graph = saved_graph;
    FeatureInstanceView instances[]{s, s};
    CHECK(!assembleFeatures(instances, {}));
    instances[1].instance_key = "two";
    CHECK(assembleFeatures(instances, {}));
    d_graph.build = +[](const void*, std::string_view instance, std::span<const ResolvedResource>,
        RenderGraphBuilder& b) noexcept -> RenderResult<void> {
        const auto key = [&](std::string_view local) {
            return featureKey(FeatureName("test.type"), instance, local);
        };
        auto r = b.declareResource({key("private"), GraphBufferDesc{64}});
        if (!r) return cxx::unexpected(r.error());
        if (auto p = fixture::pass(b, key("first"), {}, {*r, 1}); !p) return p;
        GraphPass second{key("second")};
        second.uses.push_back({{*r, 2}, {}, EGraphAccess::READ_WRITE, 1});
        if (auto p = b.addPass(std::move(second)); !p) return cxx::unexpected(p.error());
        return b.exportOutput({{*r, 2}});
    };
    auto private_instances = assembleFeatures(instances, {});
    CHECK(private_instances && private_instances->definition.resources().size() == 2);
    CHECK(private_instances->analysis.execution_order.size() == 4);
    CHECK(private_instances->definition.outputs()[0].version.resource !=
        private_instances->definition.outputs()[1].version.resource);
    instances[1].descriptor = &saved;
    CHECK(!assembleFeatures(instances, {}));
    CHECK(featureKey(FeatureName("a:b"), "c", "d") !=
        featureKey(FeatureName("a"), "b:c", "d"));
    CHECK(renderFeatureErrorDescriptors().size() == 6);
    return 0;
}

static int config()
{
    auto owned = ConfigValue::make(fixture::Config{});
    CHECK(owned);
    FeatureDescriptor d{FeatureName("test.configured"), cppType<fixture::Config>()};
    FeatureGraphOps d_graph{emptyDescribe, nullptr};
    FeatureInstanceView s{&d, &d_graph, &*owned, "one"};
    CHECK(assembleFeatures({&s, 1}, owned->retainedBytes()));
    CHECK(!assembleFeatures({&s, 1}, owned->retainedBytes() - 1));
    s.config = nullptr;
    CHECK(!assembleFeatures({&s, 1}, fixture::limits.max_encoded_bytes));
    s.config = &*owned;
    d.config = {};
    CHECK(!assembleFeatures({&s, 1}, fixture::limits.max_encoded_bytes));
    for (unsigned fault = 0; fault < 3; ++fault)
    {
        d.config = cppType<fixture::Config>();
        if (fault == 0) d.config.token = cxx::typeToken<std::uint64_t>();
        if (fault == 1) ++d.config.size;
        if (fault == 2) d.config.alignment *= 2;
        CHECK(!assembleFeatures({&s, 1}, fixture::limits.max_encoded_bytes));
    }
    d.config = cppType<fixture::Config>();
    auto wrong = ConfigValue::make(std::uint64_t{});
    CHECK(wrong);
    s.config = &*wrong;
    CHECK(!assembleFeatures({&s, 1}, fixture::limits.max_encoded_bytes));

    // Historical byte/Schema negatives remain at the real G2 persistent-data boundary.
    // Feature configuration itself has no encoding or artificial version contract.
    auto bytes = encodeData(fixture::Config{}, {}, fixture::limits);
    CHECK(bytes);
    const auto& schema = DataSchema<fixture::Config>::describe();
    for (unsigned fault = 0; fault < 6; ++fault)
    {
        auto changed = schema;
        std::vector<DataField> fields(schema.fields.begin(), schema.fields.end());
        if (fault == 0) ++changed.schema_version.minor;
        if (fault == 1) ++changed.encoding_version.major;
        if (fault == 2) changed.type = DataTypeIdView("test.other");
        if (fault == 3) changed.alignment = 8;
        if (fault == 4) { fields[0].scalar = cxx::EScalarKind::F32; changed.fields = fields; }
        if (fault == 5) ++changed.fixed_size;
        RetainableEncodedDataView bad{changed, *bytes, {}};
        CHECK(!decodeData<fixture::Config>(bad, fixture::limits));
    }
    for (unsigned fault = 0; fault < 3; ++fault)
    {
        std::vector<std::byte> raw(bytes->view().begin(), bytes->view().end());
        if (fault == 0) raw.pop_back();
        if (fault == 1) raw.push_back(std::byte{0});
        if (fault == 2) raw[12] = std::byte{2};
        auto b = lux::cxx::SharedBytes<>::copyOf(raw);
        RetainableEncodedDataView bad{schema, b, {}};
        CHECK(!decodeData<fixture::Config>(bad, fixture::limits));
    }
    // Validate attachment ownership/ranges through the real G2 reference schema.
    static constexpr DataField refs[]{
        {"external", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL, 4}
    };
    static constexpr DataSchemaDescriptor rs{
        DataTypeIdView("test.references"), {1, 0}, {1, 0}, refs, 20, 4
    };
    for (unsigned fault = 0; fault < 5; ++fault)
    {
        std::array<std::byte, 20> data{};
        cxx::BinarySpanWriter writer(data);
        writer.writeUnsigned<std::uint32_t>(fault == 1 ? 1 : 0);
        writer.writeUnsigned<std::uint64_t>(fault == 2 ? UINT64_MAX - 3 : 0);
        writer.writeUnsigned<std::uint64_t>(fault == 3 ? 32 : 16);
        auto b = lux::cxx::SharedBytes<>::copyOf(data);
        const std::array owners{*bytes};
        RetainableEncodedDataView view{rs, b, owners};
        auto r = validateEncodedData(view, {64, fault == 4 ? 15u : 64u});
        CHECK(static_cast<bool>(r) == (fault == 0));
    }
    return 0;
}

namespace negative
{
    thread_local std::vector<ResourceClaim> provided, required;
    thread_local unsigned mode = 0;
    thread_local bool require_scene = false;
    inline constexpr FeatureName provider_type("test.provider"), consumer_type("test.consumer");

    RenderResult<void> provide(const void*, std::string_view, ResourceClaims& out) noexcept
    {
        for (const auto& claim : provided)
            (claim.storage ? out.storage.global : out.resources).push_back(claim);
        if (mode == 10) return cxx::unexpected(RenderError{kInvalidEncoding, {101}});
        return {};
    }

    RenderResult<void> require(const void*, std::string_view, ResourceClaims& out) noexcept
    {
        for (const auto& claim : required)
            (claim.storage ? (require_scene ? out.storage.scene : out.storage.global) : out.resources).push_back(claim);
        return {};
    }

    RenderResult<void> produce(
        const void*, std::string_view instance, std::span<const ResolvedResource> bindings, RenderGraphBuilder& b
    ) noexcept
    {
        auto* port = findResource(bindings, "r");
        if (!port || port->version.number == 0 || mode == 11 || mode == 12) return {};
        std::vector<GraphVersion> reads;
        if (mode == 5) reads.push_back({{1}, 1}); // Consumer-owned scratch.
        auto r = fixture::pass(b, featureKey(provider_type, instance, "write"), reads, port->version);
        if (!r) return r;
        if (mode == 9) return cxx::unexpected(RenderError{kInvalidEncoding, {102}});
        if (mode == 8) return fixture::pass(b, featureKey(provider_type, instance, "write"), {}, port->version);
        return {};
    }

    RenderResult<void> consume(
        const void*, std::string_view instance, std::span<const ResolvedResource> bindings, RenderGraphBuilder& b
    ) noexcept
    {
        auto* port = findResource(bindings, "r");
        const auto key = [&](std::string_view name) { return featureKey(consumer_type, instance, name); };
        auto local = b.declareResource({mode == 7 ? "bad.private.name" : key("local"), GraphBufferDesc{64}});
        if (!local) return cxx::unexpected(local.error());
        GraphPass p{mode == 6 ? "bad.pass.name" : key("read")};
        if (port)
        {
            auto version = port->version;
            if (mode == 2) ++version.number;
            p.uses.push_back({version, {}, mode == 1 || mode == 11 ? EGraphAccess::WRITE : EGraphAccess::READ});
            if (mode == 3)
            {
                p.uses.back().access = EGraphAccess::READ_WRITE;
                p.uses.back().version.number = 2;
                p.uses.back().input_version = 1;
            }
        }
        p.uses.push_back({{*local, 1}, {}, EGraphAccess::WRITE});
        auto added = b.addPass(std::move(p));
        if (!added) return cxx::unexpected(added.error());
        if (mode == 13 && port) return b.exportOutput({port->version});
        return b.exportOutput({{*local, 1}});
    }

    RenderResult<FeatureAssembly> run()
    {
        static const FeatureDescriptor p{provider_type, {}};
        static const FeatureGraphOps p_graph{provide, produce};
        static const FeatureDescriptor c{consumer_type, {}};
        static const FeatureGraphOps c_graph{require, consume};
        const FeatureInstanceView selected[]{{&p, &p_graph, nullptr, "one"}, {&c, &c_graph, nullptr, "one"}};
        return assembleFeatures(selected, fixture::limits.max_encoded_bytes);
    }

    void reset()
    {
        mode = 0;
        require_scene = false;
        provided = {{"r", EResourceClaim::PROVIDE, GraphBufferDesc{64}}};
        required = provided;
        required[0].participation = EResourceClaim::REQUIRE;
    }
}

static int claims()
{
    using namespace negative;
    reset();
    CHECK(run());
    provided.clear();
    auto missing = run();
    CHECK(!missing && missing.error().type == kFeatureMissingProvider);
    required[0].participation = EResourceClaim::OPTIONAL;
    CHECK(run());
    reset();
    provided.push_back(provided[0]);
    CHECK(!run());
    for (unsigned fault = 0; fault < 9; ++fault)
    {
        reset();
        if (fault == 0) required[0].description = GraphBufferDesc{65};
        if (fault == 1) required[0].description = GraphImageDesc{};
        if (fault == 2) required[0].origin = EGraphOrigin::IMPORTED;
        if (fault == 3) required[0].initial_ranges = {GraphBufferRange{0, 64}};
        if (fault >= 4)
        {
            provided[0] = fixture::material(EResourceClaim::PROVIDE);
            provided[0].key = "r";
            required[0] = provided[0];
            required[0].participation = EResourceClaim::REQUIRE;
            if (fault == 4) require_scene = true;
            if (fault == 5) required[0].storage->key = "different-instance";
            if (fault == 6) required[0].storage->type = StorageName("test.OtherStorage");
            if (fault == 7) required[0].storage.reset();
            if (fault == 8) required[0].initial_ranges = {GraphBufferRange{0, 128}};
        }
        auto mismatch = run();
        CHECK(!mismatch && mismatch.error().type == kFeatureResourceMismatch);
    }
    reset();
    provided[0] = fixture::material(EResourceClaim::PROVIDE);
    provided[0].key = "r";
    required[0] = provided[0];
    required[0].participation = EResourceClaim::REQUIRE;
    required[0].initial_ranges = {GraphBufferRange{0, 256}, GraphBufferRange{0, 256}};
    CHECK(run()); // Whole-range spelling and duplicates normalize.
    provided[0].storage->config = *ConfigValue::make(std::uint32_t{31});
    required[0].storage->config = *ConfigValue::make(std::uint32_t{31});
    CHECK(run()); // Separate immutable values compare semantically, not by owner address.
    required[0].storage->config = *ConfigValue::make(std::uint32_t{32});
    auto config_conflict = run();
    CHECK(!config_conflict && config_conflict.error().type == kFeatureResourceMismatch);
    required[0].storage->config = *ConfigValue::make(float{31});
    CHECK(!run()); // Equal-looking values of a different C++ type are not interchangeable.
    required[0].storage->config = {};
    CHECK(!run()); // Missing configuration does not silently replace the provider's configuration.
    reset();
    provided[0].description = GraphBufferDesc{0};
    required[0].description = provided[0].description;
    CHECK(!run());
    reset();
    provided[0].storage = StorageRequirement{StorageName("bad")};
    CHECK(!run()); // Persistent storage cannot be transient.
    for (unsigned fault = 0; fault < 3; ++fault)
    {
        reset();
        provided[0].description = GraphImageDesc{64, 64};
        required[0].description = provided[0].description;
        auto& image = std::get<GraphImageDesc>(required[0].description);
        if (fault == 0) image.width = 32;
        if (fault == 1) image.format = lux::rdesc::ETextureFormat::R8_UNORM;
        if (fault == 2) image.samples = 4;
        CHECK(!run());
    }
    return 0;
}

static int fallback()
{
    static const FeatureDescriptor provider{FeatureName("test.history"), {}};
    static const FeatureGraphOps provider_graph{+[](const void*, std::string_view, ResourceClaims& out) noexcept -> RenderResult<void> {
            auto claim = fixture::history("previous", EResourceClaim::PROVIDE, true);
            claim.initial_ranges.clear();
            out.storage.view.push_back(std::move(claim));
            return {};
        }, nullptr};
    static const FeatureDescriptor consumer{FeatureName("test.fallback"), {}};
    static const FeatureGraphOps consumer_graph{+[](const void*, std::string_view, ResourceClaims& out) noexcept -> RenderResult<void> {
            auto claim = fixture::history("previous", EResourceClaim::REQUIRE, true);
            claim.initial_ranges.clear();
            out.storage.view.push_back(std::move(claim));
            return {};
        }, +[](const void*, std::string_view instance, std::span<const ResolvedResource> bindings,
            RenderGraphBuilder& b) noexcept -> RenderResult<void> {
            const auto key = [&](std::string_view local) {
                return featureKey(FeatureName("test.fallback"), instance, local);
            };
            auto source = b.declareResource({key("source"), GraphImageDesc{64, 64}});
            auto output = b.declareResource({key("output"), GraphImageDesc{64, 64}});
            if (!source || !output) return cxx::unexpected(RenderError{kFeatureContract});
            if (auto r = fixture::pass(b, key("initialize"), {}, {*source, 1}); !r) return r;
            GraphPass pass{key("read")};
            auto version = findResource(bindings, "previous")->version;
            const GraphResourceId fallback_source = instance == "foreign" ? GraphResourceId{1} : *source;
            pass.uses.push_back({version, {}, EGraphAccess::READ, 0, GraphVersion{fallback_source, 1}});
            pass.uses.push_back({{*output, 1}, {}, EGraphAccess::WRITE});
            if (auto r = b.addPass(std::move(pass)); !r) return cxx::unexpected(r.error());
            return b.exportOutput({{*output, 1}});
        }};
    const FeatureInstanceView selected[]{{&provider, &provider_graph, nullptr, "one"}, {&consumer, &consumer_graph, nullptr, "one"}};
    auto result = assembleFeatures(selected, {});
    CHECK(result && result->analysis.execution_order.size() == 2);
    CHECK(std::ranges::any_of(result->analysis.versions, [](const GraphVersionUse& u) { return u.fallback; }));
    const FeatureDescriptor foreign{FeatureName("test.aaa"), {}};
    const FeatureGraphOps foreign_graph{emptyDescribe, +[](const void*, std::string_view instance, std::span<const ResolvedResource>,
            RenderGraphBuilder& b) noexcept -> RenderResult<void> {
            auto name = featureKey(FeatureName("test.aaa"), instance, "private");
            auto r = b.declareResource({name, GraphImageDesc{64, 64}});
            if (!r) return cxx::unexpected(r.error());
            return fixture::pass(b, name, {}, {*r, 1});
        }};
    const FeatureInstanceView bad[]{
        {&provider, &provider_graph, nullptr, "one"}, {&consumer, &consumer_graph, nullptr, "foreign"}, {&foreign, &foreign_graph, nullptr, "one"}
    };
    auto rejected = assembleFeatures(bad, {});
    CHECK(!rejected && rejected.error().type == kFeatureOwnership);
    return 0;
}

static int ownership()
{
    using namespace negative;
    for (unsigned fault : {1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u})
    {
        reset();
        auto old = run();
        CHECK(old);
        auto old_analysis = old->analysis;
        mode = fault;
        auto r = run();
        CHECK(!r);
        CHECK(old->analysis == old_analysis && old->definition.outputs().size() == 1);
        if (fault == 1 || fault == 5 || fault == 11 || fault == 13) CHECK(r.error().type == kFeatureOwnership);
        if (fault == 2 || fault == 3) CHECK(r.error().type == kFeatureVersion || r.error().type == kFeatureOwnership);
        if (fault == 8) CHECK(r.error().type == kGraphDuplicateKey);
        if (fault == 9) CHECK(r.error().type == kInvalidEncoding && r.error().args[0] == 102);
        if (fault == 10) CHECK(r.error().type == kInvalidEncoding && r.error().args[0] == 101);
        if (fault == 12) CHECK(r.error().type == kGraphUndefinedRead);
    }
    return 0;
}

static int lifetime()
{
    FeatureAssembly retained;
    {
        std::string name = "owned.dynamic.material.type";
        negative::reset();
        negative::provided[0] = fixture::material(EResourceClaim::PROVIDE);
        negative::provided[0].key = "r";
        negative::provided[0].storage->type = StorageName(name);
        negative::required[0] = negative::provided[0];
        negative::required[0].participation = EResourceClaim::REQUIRE;
        auto result = negative::run();
        CHECK(result);
        retained = std::move(*result);
    }
    negative::provided.clear();
    negative::required.clear();
    CHECK(retained.external_bindings.global[0].storage.type.name() == "owned.dynamic.material.type");
    auto moved = std::move(retained);
    CHECK(analyze(moved.definition)->execution_order == moved.analysis.execution_order);
    const auto descriptions = fixture::descriptions.load();
    const auto builds = fixture::builds.load();
    auto result = fixture::scenario(2);
    CHECK(result);
    CHECK(fixture::descriptions.load() == descriptions + 2 && fixture::builds.load() == builds + 1);
    for (unsigned i = 0; i < 100; ++i) CHECK(!result->analysis.execution_order.empty());
    CHECK(fixture::builds.load() == builds + 1);
    // A temporary Definition/config/instance and counted byte owner all die;
    // the returned graph alone retains the copied resource/pass keys.
    std::atomic<unsigned> released{0};
    FeatureAssembly configured;
    {
        auto owner = std::shared_ptr<const fixture::Config>(new fixture::Config{}, [&](auto* value) noexcept {
            delete value;
            ++released;
        });
        auto config = ConfigValue::make(CountedConfig{owner});
        CHECK(config);
        std::string type_name = "test.temporary";
        FeatureDescriptor d{FeatureName(type_name), cppType<CountedConfig>()};
        FeatureGraphOps d_graph{emptyDescribe, +[](const void* wire, std::string_view instance, std::span<const ResolvedResource>,
                RenderGraphBuilder& b) noexcept -> RenderResult<void> {
                const auto* c = static_cast<const CountedConfig*>(wire)->value.get();
                auto name = featureKey(FeatureName("test.temporary"), instance, std::to_string(c->first));
                auto r = b.declareResource({name, GraphBufferDesc{64}});
                if (!r) return cxx::unexpected(r.error());
                if (auto p = fixture::pass(b, name, {}, {*r, 1}); !p) return p;
                return b.exportOutput({{*r, 1}});
            }};
        std::string instance = "temporary.instance";
        FeatureInstanceView s{&d, &d_graph, &*config, instance};
        auto r = assembleFeatures({&s, 1}, fixture::limits.max_encoded_bytes);
        CHECK(r && released == 0);
        configured = std::move(*r);
    }
    CHECK(released == 1);
    CHECK(configured.definition.resources()[0].key.find("temporary.instance") != std::string::npos);
    CHECK(analyze(configured.definition)->execution_order == configured.analysis.execution_order);
    return 0;
}

static int concurrency()
{
    std::barrier start(4);
    std::atomic<bool> good{true};
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            start.arrive_and_wait();
            for (unsigned n = 0; n < 100; ++n)
            {
                auto r = fixture::scenario(n % 3);
                if (!r || r->analysis.execution_order.empty()) good = false;
            }
        });
    threads.clear();
    CHECK(good);
    return 0;
}

namespace dependency_test
{
    thread_local unsigned mode = 0;
    thread_local bool require_scene = false;

    RenderResult<void> first(const void*, std::string_view instance, std::span<const ResolvedResource>,
        RenderGraphBuilder& b) noexcept
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            auto p = b.addPass({featureKey(FeatureName("test.edge.a"), instance, std::to_string(i)),
                EGraphPassKind::COMPUTE, {}, true});
            if (!p) return cxx::unexpected(p.error());
        }
        if (mode == 3) return b.dependsOn({0}, {1});
        return {};
    }

    RenderResult<void> second(const void*, std::string_view instance, std::span<const ResolvedResource>,
        RenderGraphBuilder& b) noexcept
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            auto p = b.addPass({featureKey(FeatureName("test.edge.b"), instance, std::to_string(i)),
                EGraphPassKind::COMPUTE, {}, true});
            if (!p) return cxx::unexpected(p.error());
        }
        if (mode == 0 || mode == 3) return b.dependsOn({0}, {1});
        if (mode == 1) return b.dependsOn({1}, {2});
        return b.dependsOn({2}, {3});
    }
}

static int dependencies()
{
    const FeatureDescriptor a{FeatureName("test.edge.a"), {}};
    const FeatureGraphOps a_graph{emptyDescribe, dependency_test::first};
    const FeatureDescriptor b{FeatureName("test.edge.b"), {}};
    const FeatureGraphOps b_graph{emptyDescribe, dependency_test::second};
    const FeatureInstanceView selected[]{{&b, &b_graph, nullptr, "main"}, {&a, &a_graph, nullptr, "main"}};
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        dependency_test::mode = mode;
        auto result = assembleFeatures(selected, fixture::limits.max_encoded_bytes);
        if (mode < 2)
        {
            CHECK(!result && result.error().type == kFeatureOwnership);
        }
        else
        {
            CHECK(result && result->definition.dependencies().size() == 1);
            CHECK(result->analysis.execution_order.size() == 4);
        }
    }
    return 0;
}

static int counters()
{
    RenderGraphBuilder b;
    CHECK(b.resourceCount() == 0 && b.passCount() == 0 && b.outputCount() == 0);
    CHECK(!b.declareResource({"bad", GraphBufferDesc{0}}) && b.resourceCount() == 0);
    auto r = b.declareResource({"buffer", GraphBufferDesc{64}});
    CHECK(r && b.resourceCount() == 1);
    CHECK(!b.addPass({"bad", EGraphPassKind::COMPUTE, {{{{99}, 1}, {}, EGraphAccess::WRITE}}}));
    CHECK(b.passCount() == 0);
    CHECK(fixture::pass(b, "good", {}, {*r, 1}) && b.passCount() == 1);
    CHECK(!b.exportOutput({{{99}, 1}}) && b.outputCount() == 0);
    CHECK(b.exportOutput({{*r, 1}}) && b.outputCount() == 1);
    CHECK(b.dependencyCount() == 0);
    CHECK(!b.dependsOn({0}, {99}) && b.dependencyCount() == 0);
    CHECK(!b.dependsOn({0}, {0}) && b.dependencyCount() == 0);
    CHECK(b.addPass({"second", EGraphPassKind::COMPUTE, {}, true}));
    CHECK(b.dependsOn({0}, {1}) && b.dependencyCount() == 1);
    CHECK(b.dependsOn({0}, {1}) && b.dependencyCount() == 1);
    CHECK(std::move(b).finish());
    CHECK(b.resourceCount() == 0 && b.passCount() == 0 && b.outputCount() == 0);
    CHECK(b.dependencyCount() == 0);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view group = argv[1];
    if (group == "composition") return composition();
    if (group == "identity") return identity();
    if (group == "config") return config();
    if (group == "claims") return claims();
    if (group == "ownership") return ownership();
    if (group == "fallback") return fallback();
    if (group == "lifetime") return lifetime();
    if (group == "concurrency") return concurrency();
    if (group == "counters") return counters();
    if (group == "dependencies") return dependencies();
    return 3;
}
