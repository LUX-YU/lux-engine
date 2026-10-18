#include <lux/engine/render/protocol/CommandMethod.hpp>

#include <cstdio>
#include <string>
#include <vector>
#include <lux/cxx/core/CheckedArithmetic.hpp>

using namespace lux::render;
namespace cxx = lux::cxx;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

struct Query final
{
    static constexpr std::string_view name = "test.Query";
    struct Request final { std::uint64_t key = 0; };
    using Response = RenderResult<std::vector<std::uint32_t>>;
};

struct Signal final
{
    static constexpr std::string_view name = "test.Signal";
    using Request = Ack;
    using Response = RenderResult<Ack>;
};

struct MissingResponse final
{
    static constexpr std::string_view name = "test.MissingResponse";
    using Request = Ack;
};

struct ThrowingMove final
{
    ThrowingMove(ThrowingMove&&) noexcept(false);
};

struct InvalidCommand final
{
    static constexpr std::string_view name = "test.InvalidCommand";
    using Request = ThrowingMove;
    using Response = RenderResult<Ack>;
};

static_assert(RenderCommand<Query> && RenderCommand<Signal>);
static_assert(!RenderCommand<MissingResponse> && !RenderCommand<InvalidCommand>);

static int descriptor()
{
    const auto& reference = commandDescriptor<Query>();
    CHECK(validateCommand(reference));
    CHECK(reference.name.name() == Query::name);
    CHECK(reference.request.token == cxx::typeToken<Query::Request>());
    CHECK(reference.response.token == cxx::typeToken<Query::Response>());
    CHECK(reference.request.size == sizeof(Query::Request));
    CHECK(reference.response.alignment == alignof(Query::Response));
    const auto& signal = commandDescriptor<Signal>();
    CHECK(signal.name.name() == Signal::name);
    CHECK(signal.response.token == cxx::typeToken<Signal::Response>());
    CHECK(compatibleCommands(reference, reference));
    CHECK(CommandName::fromVerified(reference.name.name(), reference.name.hash()) == reference.name);
    const CommandName other_name("test.Other");
    CHECK(other_name.hash() != reference.name.hash());
    CHECK(!CommandName::fromVerified(other_name.name(), reference.name.hash()).isValid());
    // Primary collision vectors, independently recomputed with the unchanged lux-cxx Fnv1a64:
    // https://github.com/pstibrany/fnv-1a-64bit-collisions/blob/4dac53c19e5d53fbfb8d1050af37524b3313acf8/README.md
    constexpr CommandName collision_a("gMPflVXtwGDXbIhP73TX");
    constexpr CommandName collision_b("LtHf1prlU1bCeYZEdqWf");
    static_assert(collision_a.hash() == 0x8FCF3BE2DE898214ULL);
    static_assert(collision_a.hash() == collision_b.hash() && collision_a != collision_b);
    CHECK(CommandName::fromVerified(collision_a.name(), collision_b.hash()) == collision_a);
    CHECK(CommandName::fromVerified(collision_b.name(), collision_a.hash()) == collision_b);
    auto colliding_left = reference;
    auto colliding_right = reference;
    colliding_left.name = collision_a;
    colliding_right.name = collision_b;
    CHECK(validateCommand(colliding_left) && validateCommand(colliding_right));
    CHECK(compatibleCommands(colliding_left, colliding_left));
    CHECK(compatibleCommands(colliding_right, colliding_right));
    const auto collision_mismatch = compatibleCommands(colliding_left, colliding_right);
    CHECK(!collision_mismatch && collision_mismatch.error().type == kProtocolContract);
    CHECK(!compatibleCommands(colliding_right, colliding_left));
    std::puts("CommandName: real FNV1a64 collision 0x8FCF3BE2DE898214 retains distinct full-name identity");

    std::string copied_name(reference.name.name());
    std::string copied_request(reference.request.token.name());
    auto equivalent = reference;
    equivalent.name = CommandName(copied_name);
    equivalent.request.token = cxx::TypeToken(reference.request.token.hash(), copied_request);
    CHECK(compatibleCommands(reference, equivalent));
    for (unsigned fault = 0; fault < 10; ++fault)
    {
        auto changed = reference;
        if (fault == 0) changed.name = CommandName("test.Other");
        if (fault == 1) changed.request.token = cxx::typeToken<std::uint64_t>();
        if (fault == 2) ++changed.request.size;
        if (fault == 3) changed.request.alignment *= 2;
        if (fault == 4) changed.response.token = cxx::typeToken<Ack>();
        if (fault == 5) changed.request.token = cxx::TypeToken(changed.request.token.hash(), "same-hash-other-name");
        if (fault == 6) changed.request.token = cxx::TypeToken(0, changed.request.token.name());
        if (fault == 7) changed.response.token = cxx::TypeToken(changed.response.token.hash(), "other-response");
        if (fault == 8) ++changed.response.size;
        if (fault == 9) changed.response.alignment *= 2;
        auto mismatch = compatibleCommands(reference, changed);
        CHECK(!mismatch && mismatch.error().type == kProtocolContract);
    }
    for (unsigned fault = 0; fault < 7; ++fault)
    {
        auto changed = reference;
        if (fault == 0) changed.name = {};
        if (fault == 1) changed.response.size = 1;
        if (fault == 2) changed.request.token = {};
        if (fault == 3) changed.request.size = 0;
        if (fault == 4) changed.response.alignment = 0;
        if (fault == 5) changed.response.alignment = 3;
        if (fault == 6) changed.response.token = {};
        CHECK(!validateCommand(changed));
    }
    return 0;
}


int main()
{
    return descriptor();
}
