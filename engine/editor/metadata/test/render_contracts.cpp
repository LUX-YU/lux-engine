#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <cassert>
#include <deque>
#include <string>

using namespace lux::render;
using Dispatcher = GeneralRenderServer::Dispatcher;
struct Payload { std::uint32_t value{}; };
void ignore(Dispatcher::Ctx &, const Payload &) {}
struct NamedOp
{
    using Payload = ::Payload;
    static constexpr auto kind = EOpKind::Stream;
    static constexpr auto lane = EOperationLane::Program;
    static constexpr const char *name = "test.duplicate";
};
template<std::size_t> struct Param
{
    using Payload = SetFeatureParamsPayload;
    static constexpr auto kind = EOpKind::Param;
    static constexpr auto lane = EOperationLane::Program;
    static constexpr const char *name = "";
};
template<std::size_t... I> auto registrar(std::index_sequence<I...>) -> FeatureOpRegistrar<ServerOp<Param<I>>...>;

int main()
{
    using Sixteen = decltype(registrar(std::make_index_sequence<16>{}));
    using Seventeen = decltype(registrar(std::make_index_sequence<17>{}));
    Dispatcher dispatcher;
    TypeId ids[17]{};
    assert(!Seventeen::registerAll(&dispatcher, ids, 17));
    const auto accepted = Sixteen::registerAll(&dispatcher, ids, 16);
    assert(accepted && *accepted == 16 && typeIdIndex(ids[0]) == 0);
    Sixteen::unregisterAll(&dispatcher, ids, 16);
    using Duplicated = FeatureOpRegistrar<ServerOp<NamedOp, &ignore>, ServerOp<NamedOp, &ignore>>;
    assert(!Duplicated::registerAll(&dispatcher, ids, 16));
    assert(dispatcher.findTypeId(NamedOp::name).type_id == kInvalidTypeId);
    const auto first = dispatcher.allocateAndRegisterUnary<Payload, &ignore>(opcodes::CommandOp, NamedOp::name);
    assert(first != kInvalidTypeId);
    assert((dispatcher.allocateAndRegisterUnary<Payload, &ignore>(opcodes::CommandOp, NamedOp::name) == kInvalidTypeId));
    assert(dispatcher.findTypeId(NamedOp::name).type_id == first);

    // All 16-bit indices are representable; the following allocation fails without wrapping.
    Dispatcher capacity;
    for (std::uint32_t i{}; i <= UINT16_MAX; ++i)
    {
        const auto id = capacity.allocateAndRegisterUnary<Payload, &ignore>(opcodes::CommandOp);
        assert(id != kInvalidTypeId && typeIdIndex(id) == i);
    }
    assert((capacity.allocateAndRegisterUnary<Payload, &ignore>(opcodes::CommandOp) == kInvalidTypeId));
    Dispatcher generations;
    for (std::uint32_t i{1}; i <= UINT16_MAX; ++i)
    {
        const auto id = generations.allocateAndRegisterUnary<Payload, &ignore>(opcodes::CommandOp);
        assert(typeIdIndex(id) == 0 && typeIdGen(id) == i);
        generations.freeSlot(opcodes::CommandOp, id);
    }
    assert(typeIdIndex(generations.allocateAndRegisterUnary<Payload, &ignore>(opcodes::CommandOp)) == 1);

    FeatureCatalog catalog;
    std::deque<std::string> names;
    const FeatureCatalog::Entry *stable{};
    for (unsigned i{}; i < 1024; ++i)
    {
        names.push_back("test.feature." + std::to_string(i));
        FeatureFactory factory;
        factory.name = names.back().c_str();
        factory.descriptor = {.type = featureId(names.back()), .name = names.back(),
                              .abi_version = 1, .canonical_name = names.back()};
        assert(catalog.add(factory, i + 1, {}));
        if (i == 0) stable = catalog.find(names.front());
        assert(catalog.find(names.front()) == stable && stable->name == names.front());
    }
    const std::array selection{std::string_view{names[900]}, std::string_view{names[0]}};
    assert(catalog.resolveAttachOrder(selection).order == std::vector<std::string_view>(selection.begin(), selection.end()));
    FeatureCatalog relations;
    const std::array dep_b{FeatureDependency{featureId("b"), false, 1}};
    const std::array dep_a{FeatureDependency{featureId("a"), false, 1}};
    FeatureFactory a, b;
    a.name = "a"; a.descriptor = {.type = featureId("a"), .name = "a", .dependencies = dep_b};
    b.name = "b"; b.descriptor = {.type = featureId("b"), .name = "b", .dependencies = dep_a};
    assert(relations.add(a, 1, {}) && relations.add(b, 2, {}));
    assert(!relations.resolveAttachOrder(std::array{std::string_view{"a"}}).missing_deps.empty());
    assert(!relations.resolveAttachOrder(std::array{std::string_view{"a"}, std::string_view{"b"}}).cycle.empty());
    a.descriptor.dependencies = {};
    b.descriptor.dependencies = {};
    const std::array conflict_b{featureId("b")};
    a.descriptor.conflicts = conflict_b;
    FeatureCatalog conflicts;
    assert(conflicts.add(a, 1, {}) && conflicts.add(b, 2, {}));
    const std::array pair{std::string_view{"a"}, std::string_view{"b"}};
    assert(conflicts.resolveAttachOrder(pair).conflicts.size() == 1);
    a.descriptor.conflicts = {};
    FeatureCatalog forward, reverse;
    assert(forward.add(a, 1, {}) && forward.add(b, 2, {}));
    assert(reverse.add(b, 2, {}) && reverse.add(a, 1, {}));
    assert(forward.resolveAttachOrder(pair).order == reverse.resolveAttachOrder(pair).order);
}
