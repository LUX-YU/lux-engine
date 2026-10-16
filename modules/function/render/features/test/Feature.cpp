#include "Scenarios.hpp"

#include <barrier>
#include <cstdio>
#include <thread>

using namespace lux::render;
namespace cxx = lux::cxx;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

static RenderResult<void> emptyDescribe(const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept
{
    return {};
}

static int composition()
{
    using enum fixture::ERole;
    const std::array types{&fixture::definition<GEOMETRY>(), &fixture::definition<LIGHTING>(),
        &fixture::definition<TONEMAP>()};
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
        std::vector<SelectedFeature> selected;
        for (auto i : order) selected.push_back({types[i], nullptr, "main"});
        auto result = assembleFeatures(selected, fixture::limits);
        CHECK(result && result->analysis == reference->analysis);
        for (std::size_t i = 0; i < result->definition.resources().size(); ++i)
            CHECK(result->definition.resources()[i].key == reference->definition.resources()[i].key);
        for (std::size_t i = 0; i < result->definition.passes().size(); ++i)
            CHECK(result->definition.passes()[i].key == reference->definition.passes()[i].key);
    } while (std::next_permutation(order.begin(), order.end()));
    const SelectedFeature alternate[]{
        {types[0], nullptr, "main"}, {&fixture::definition<ALTERNATE>(), nullptr, "main"},
        {types[2], nullptr, "main"}
    };
    auto alt = assembleFeatures(alternate, fixture::limits);
    CHECK(alt && alt->analysis.execution_order.size() == 5);
    const SelectedFeature conflict[]{
        alternate[0], alternate[1], alternate[2], {types[1], nullptr, "main"}
    };
    auto duplicate = assembleFeatures(conflict, fixture::limits);
    CHECK(!duplicate && duplicate.error().type == kFeatureDuplicate);
    auto hzb = fixture::scenario(1);
    CHECK(hzb && hzb->external_bindings.size() == 2 && hzb->analysis.execution_order.size() == 1);
    for (const auto& b : hzb->external_bindings) CHECK(b.storage.scope == EResourceScope::VIEW);
    const auto& hr = hzb->definition.resources();
    CHECK(hr.size() == 2 && hr[0].origin == EGraphOrigin::HISTORY_CURRENT);
    CHECK(hr[1].origin == EGraphOrigin::HISTORY_PREVIOUS);
    CHECK(hzb->definition.passes()[0].uses[0].version.number == 0);
    CHECK(hzb->definition.passes()[0].uses[0].version.resource !=
        hzb->definition.passes()[0].uses[1].version.resource);
    auto material = fixture::scenario(2);
    CHECK(material && material->external_bindings.size() == 1);
    CHECK(material->definition.resources().size() == 2 && material->definition.passes().size() == 1);
    CHECK(material->external_bindings[0].storage.type.name() == "test.MaterialStorage");
    CHECK(material->definition.passes()[0].uses[0].version.number == 0);
    return 0;
}

static int identity()
{
    auto empty = assembleFeatures({}, {});
    CHECK(empty && empty->analysis.execution_order.empty());
    FeatureDefinition d{FeatureTypeIdView("test.type"), {1, 0}, nullptr, emptyDescribe};
    SelectedFeature s{&d, nullptr, "one"};
    CHECK(assembleFeatures({&s, 1}, {}));
    SelectedFeature missing{};
    CHECK(!assembleFeatures({&missing, 1}, {}));
    s.instance_key = "";
    CHECK(!assembleFeatures({&s, 1}, {}));
    s.instance_key = "one";
    auto saved = d;
    d.type = {};
    CHECK(!assembleFeatures({&s, 1}, {}));
    d = saved;
    d.version = {};
    CHECK(!assembleFeatures({&s, 1}, {}));
    d = saved;
    d.describe = nullptr;
    CHECK(!assembleFeatures({&s, 1}, {}));
    d = saved;
    SelectedFeature instances[]{s, s};
    CHECK(!assembleFeatures(instances, {}));
    instances[1].instance_key = "two";
    CHECK(assembleFeatures(instances, {}));
    d.build = +[](const WireView*, std::string_view instance, std::span<const ResolvedResource>,
        RenderGraphBuilder& b) noexcept -> RenderResult<void> {
        const auto key = [&](std::string_view local) {
            return featureKey(FeatureTypeIdView("test.type"), instance, local);
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
    instances[1].definition = &saved;
    CHECK(!assembleFeatures(instances, {}));
    d.commands = {};
    CHECK(featureKey(FeatureTypeIdView("a:b"), "c", "d") !=
        featureKey(FeatureTypeIdView("a"), "b:c", "d"));
    CHECK(renderFeatureErrorDescriptors().size() == 6);
    return 0;
}

static int config()
{
    auto bytes = encodeWire(fixture::Config{}, {}, fixture::limits);
    CHECK(bytes);
    const auto& schema = DataSchema<fixture::Config>::describe();
    WireView wire{schema, *bytes, {}};
    FeatureDefinition d{FeatureTypeIdView("test.configured"), {1, 0}, &schema, emptyDescribe};
    SelectedFeature s{&d, &wire, "one"};
    CHECK(assembleFeatures({&s, 1}, fixture::limits));
    CHECK(!assembleFeatures({&s, 1}, {15, 4096}));
    s.config = nullptr;
    CHECK(!assembleFeatures({&s, 1}, fixture::limits));
    s.config = &wire;
    d.config_schema = nullptr;
    CHECK(!assembleFeatures({&s, 1}, fixture::limits));
    d.config_schema = &schema;
    for (unsigned fault = 0; fault < 6; ++fault)
    {
        auto changed = schema;
        std::vector<DataField> fields(schema.fields.begin(), schema.fields.end());
        if (fault == 0) ++changed.schema_version.minor;
        if (fault == 1) ++changed.wire_version.major;
        if (fault == 2) changed.type = DataTypeIdView("test.other");
        if (fault == 3) changed.alignment = 8;
        if (fault == 4) { fields[0].scalar = cxx::EScalarKind::F32; changed.fields = fields; }
        if (fault == 5) ++changed.fixed_size;
        WireView bad{changed, *bytes, {}};
        s.config = &bad;
        CHECK(!assembleFeatures({&s, 1}, fixture::limits));
    }
    for (unsigned fault = 0; fault < 3; ++fault)
    {
        std::vector<std::byte> raw(bytes->view().begin(), bytes->view().end());
        if (fault == 0) raw.pop_back();
        if (fault == 1) raw.push_back(std::byte{0});
        if (fault == 2) raw[12] = std::byte{2};
        auto b = OwnedWireBytes::copyOf(raw);
        WireView bad{schema, b, {}};
        s.config = &bad;
        CHECK(!assembleFeatures({&s, 1}, fixture::limits));
    }
    // Validate attachment ownership/ranges through the real G2 reference schema.
    static constexpr DataField refs[]{
        {"external", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL, 4}
    };
    static constexpr DataSchemaDescriptor rs{
        DataTypeIdView("test.references"), {1, 0}, {1, 0}, refs, 20, 4
    };
    d.config_schema = &rs;
    for (unsigned fault = 0; fault < 5; ++fault)
    {
        std::array<std::byte, 20> data{};
        cxx::BinarySpanWriter writer(data);
        writer.writeUnsigned<std::uint32_t>(fault == 1 ? 1 : 0);
        writer.writeUnsigned<std::uint64_t>(fault == 2 ? UINT64_MAX - 3 : 0);
        writer.writeUnsigned<std::uint64_t>(fault == 3 ? 32 : 16);
        auto b = OwnedWireBytes::copyOf(data);
        const std::array owners{*bytes};
        WireView view{rs, b, owners};
        s.config = &view;
        auto r = assembleFeatures({&s, 1}, {64, fault == 4 ? 15u : 64u});
        CHECK(static_cast<bool>(r) == (fault == 0));
    }
    return 0;
}

struct ConfigureFixture final
{
    static constexpr std::string_view name = "test.operation";
    using Request = fixture::Config;
    using Response = Ack;
};

static int operations()
{
    auto operation = commandDescriptor<ConfigureFixture>();
    FeatureDefinition a{FeatureTypeIdView("test.a"), {1, 0}, nullptr, emptyDescribe, nullptr, {&operation, 1}};
    FeatureDefinition b{FeatureTypeIdView("test.b"), {1, 0}, nullptr, emptyDescribe, nullptr, {&operation, 1}};
    SelectedFeature s[]{{&a, nullptr, "one"}, {&a, nullptr, "two"}};
    CHECK(assembleFeatures(s, {}));
    s[1].definition = &b;
    auto duplicate = assembleFeatures(s, {});
    CHECK(!duplicate && duplicate.error().type == kFeatureDuplicate);
    auto other = operation;
    ++other.version.minor;
    b.commands = {&other, 1};
    CHECK(!assembleFeatures(s, {}));
    other = operation;
    other.completion = ECompletionContract::RESOURCE_GPU_READY;
    CHECK(!assembleFeatures(s, {}));
    other = operation;
    other.response = other.request;
    CHECK(!assembleFeatures(s, {}));
    other.name = CommandName("test.other");
    CHECK(assembleFeatures(s, {}));
    other.version = {};
    CHECK(!assembleFeatures(s, {}));
    std::array repeated{operation, operation};
    a.commands = repeated;
    CHECK(!assembleFeatures({s, 1}, {}));
    return 0;
}

namespace negative
{
    thread_local std::vector<ResourceClaim> provided, required;
    thread_local unsigned mode = 0;
    inline constexpr FeatureTypeIdView provider_type("test.provider"), consumer_type("test.consumer");

    RenderResult<void> provide(const WireView*, std::string_view, std::vector<ResourceClaim>& out) noexcept
    {
        out = provided;
        if (mode == 10) return cxx::unexpected(RenderError{kInvalidWire, {101}});
        return {};
    }

    RenderResult<void> require(const WireView*, std::string_view, std::vector<ResourceClaim>& out) noexcept
    {
        out = required;
        return {};
    }

    RenderResult<void> produce(
        const WireView*, std::string_view instance, std::span<const ResolvedResource> bindings, RenderGraphBuilder& b
    ) noexcept
    {
        auto* port = findResource(bindings, "r");
        if (!port || port->version.number == 0 || mode == 11 || mode == 12) return {};
        std::vector<GraphVersion> reads;
        if (mode == 5) reads.push_back({{1}, 1}); // Consumer-owned scratch.
        auto r = fixture::pass(b, featureKey(provider_type, instance, "write"), reads, port->version);
        if (!r) return r;
        if (mode == 9) return cxx::unexpected(RenderError{kInvalidWire, {102}});
        if (mode == 8) return fixture::pass(b, featureKey(provider_type, instance, "write"), {}, port->version);
        return {};
    }

    RenderResult<void> consume(
        const WireView*, std::string_view instance, std::span<const ResolvedResource> bindings, RenderGraphBuilder& b
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
        static const FeatureDefinition p{provider_type, {1, 0}, nullptr, provide, produce};
        static const FeatureDefinition c{consumer_type, {1, 0}, nullptr, require, consume};
        const SelectedFeature selected[]{{&p, nullptr, "one"}, {&c, nullptr, "one"}};
        return assembleFeatures(selected, fixture::limits);
    }

    void reset()
    {
        mode = 0;
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
            if (fault == 4) required[0].storage->scope = EResourceScope::SCENE;
            if (fault == 5) ++required[0].storage->abi.minor;
            if (fault == 6) required[0].storage->type = StorageTypeId("test.OtherStorage");
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
    reset();
    provided[0].description = GraphBufferDesc{0};
    required[0].description = provided[0].description;
    CHECK(!run());
    reset();
    provided[0].storage = StorageRequirement{StorageTypeId("bad"), {1, 0}, EResourceScope::GLOBAL};
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
    static const FeatureDefinition provider{
        FeatureTypeIdView("test.history"), {1, 0}, nullptr,
        +[](const WireView*, std::string_view, std::vector<ResourceClaim>& out) noexcept -> RenderResult<void> {
            auto claim = fixture::history("previous", EResourceClaim::PROVIDE, true);
            claim.initial_ranges.clear();
            out.push_back(std::move(claim));
            return {};
        }
    };
    static const FeatureDefinition consumer{
        FeatureTypeIdView("test.fallback"), {1, 0}, nullptr,
        +[](const WireView*, std::string_view, std::vector<ResourceClaim>& out) noexcept -> RenderResult<void> {
            auto claim = fixture::history("previous", EResourceClaim::REQUIRE, true);
            claim.initial_ranges.clear();
            out.push_back(std::move(claim));
            return {};
        },
        +[](const WireView*, std::string_view instance, std::span<const ResolvedResource> bindings,
            RenderGraphBuilder& b) noexcept -> RenderResult<void> {
            const auto key = [&](std::string_view local) {
                return featureKey(FeatureTypeIdView("test.fallback"), instance, local);
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
        }
    };
    const SelectedFeature selected[]{{&provider, nullptr, "one"}, {&consumer, nullptr, "one"}};
    auto result = assembleFeatures(selected, {});
    CHECK(result && result->analysis.execution_order.size() == 2);
    CHECK(std::ranges::any_of(result->analysis.versions, [](const GraphVersionUse& u) { return u.fallback; }));
    const FeatureDefinition foreign{
        FeatureTypeIdView("test.aaa"), {1, 0}, nullptr, emptyDescribe,
        +[](const WireView*, std::string_view instance, std::span<const ResolvedResource>,
            RenderGraphBuilder& b) noexcept -> RenderResult<void> {
            auto name = featureKey(FeatureTypeIdView("test.aaa"), instance, "private");
            auto r = b.declareResource({name, GraphImageDesc{64, 64}});
            if (!r) return cxx::unexpected(r.error());
            return fixture::pass(b, name, {}, {*r, 1});
        }
    };
    const SelectedFeature bad[]{
        {&provider, nullptr, "one"}, {&consumer, nullptr, "foreign"}, {&foreign, nullptr, "one"}
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
        if (fault == 9) CHECK(r.error().type == kInvalidWire && r.error().args[0] == 102);
        if (fault == 10) CHECK(r.error().type == kInvalidWire && r.error().args[0] == 101);
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
        negative::provided[0].storage->type = StorageTypeId(name);
        negative::required[0] = negative::provided[0];
        negative::required[0].participation = EResourceClaim::REQUIRE;
        auto result = negative::run();
        CHECK(result);
        retained = std::move(*result);
    }
    negative::provided.clear();
    negative::required.clear();
    CHECK(retained.external_bindings[0].storage.type.name() == "owned.dynamic.material.type");
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
        struct Owner final
        {
            std::atomic<unsigned>* released;
            std::array<std::byte, 16> data{};
            ~Owner() { ++*released; }
        };
        auto owner = std::make_shared<Owner>();
        owner->released = &released;
        auto bytes = OwnedWireBytes::fromOwner(owner, owner->data);
        WireView view{DataSchema<fixture::Config>::describe(), bytes, {}};
        std::string type_name = "test.temporary";
        FeatureDefinition d{FeatureTypeIdView(type_name), {1, 0}, &view.schema, emptyDescribe,
            +[](const WireView* wire, std::string_view instance, std::span<const ResolvedResource>,
                RenderGraphBuilder& b) noexcept -> RenderResult<void> {
                auto c = decodeWire<fixture::Config>(*wire, fixture::limits);
                if (!c) return cxx::unexpected(c.error());
                auto name = featureKey(FeatureTypeIdView("test.temporary"), instance, std::to_string(c->first));
                auto r = b.declareResource({name, GraphBufferDesc{64}});
                if (!r) return cxx::unexpected(r.error());
                if (auto p = fixture::pass(b, name, {}, {*r, 1}); !p) return p;
                return b.exportOutput({{*r, 1}});
            }
        };
        std::string instance = "temporary.instance";
        SelectedFeature s{&d, &view, instance};
        auto r = assembleFeatures({&s, 1}, fixture::limits);
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

    RenderResult<void> first(const WireView*, std::string_view instance, std::span<const ResolvedResource>,
        RenderGraphBuilder& b) noexcept
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            auto p = b.addPass({featureKey(FeatureTypeIdView("test.edge.a"), instance, std::to_string(i)),
                EGraphPassKind::COMPUTE, {}, true});
            if (!p) return cxx::unexpected(p.error());
        }
        if (mode == 3) return b.dependsOn({0}, {1});
        return {};
    }

    RenderResult<void> second(const WireView*, std::string_view instance, std::span<const ResolvedResource>,
        RenderGraphBuilder& b) noexcept
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            auto p = b.addPass({featureKey(FeatureTypeIdView("test.edge.b"), instance, std::to_string(i)),
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
    const FeatureDefinition a{FeatureTypeIdView("test.edge.a"), {1, 0}, nullptr, emptyDescribe,
        dependency_test::first};
    const FeatureDefinition b{FeatureTypeIdView("test.edge.b"), {1, 0}, nullptr, emptyDescribe,
        dependency_test::second};
    const SelectedFeature selected[]{{&b, nullptr, "main"}, {&a, nullptr, "main"}};
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        dependency_test::mode = mode;
        auto result = assembleFeatures(selected, fixture::limits);
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
    if (group == "operations") return operations();
    if (group == "claims") return claims();
    if (group == "ownership") return ownership();
    if (group == "fallback") return fallback();
    if (group == "lifetime") return lifetime();
    if (group == "concurrency") return concurrency();
    if (group == "counters") return counters();
    if (group == "dependencies") return dependencies();
    return 3;
}
