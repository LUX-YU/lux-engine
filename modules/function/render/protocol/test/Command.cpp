#include <lux/engine/render/protocol/Protocol.hpp>

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
    using Response = std::vector<std::uint32_t>;
};

struct Signal final
{
    static constexpr std::string_view name = "test.Signal";
    static constexpr cxx::SchemaVersion version{3, 7};
    static constexpr auto completion = ECompletionContract::RESULT_AVAILABLE;
    using Request = Ack;
    using Response = Ack;
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
    using Response = Ack;
};

template <typename T>
concept HasBudget = requires(const T& value) { CommandValueTraits<T>::retainedBytes(value); };

static_assert(RenderCommand<Query> && RenderCommand<Signal>);
static_assert(!RenderCommand<MissingResponse> && !RenderCommand<InvalidCommand>);
static_assert(HasBudget<Query::Request> && !HasBudget<Query::Response>);
static_assert(sizeof(CommandInvocation) == 4 * sizeof(void*));
static_assert(sizeof(CommandContext<Query>) == sizeof(void*));
static_assert(!std::is_copy_constructible_v<CommandContext<Query>>);

static int descriptor()
{
    const auto& reference = commandDescriptor<Query>();
    CHECK(validateCommand(reference));
    CHECK(reference.name.name() == Query::name);
    CHECK(reference.request.type == cxx::typeToken<Query::Request>());
    CHECK(reference.response.type == cxx::typeToken<Query::Response>());
    CHECK(reference.request.size == sizeof(Query::Request));
    CHECK(reference.response.alignment == alignof(Query::Response));
    const auto& signal = commandDescriptor<Signal>();
    CHECK(signal.version == cxx::SchemaVersion(3, 7));
    CHECK(signal.completion == ECompletionContract::RESULT_AVAILABLE);
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
    std::string copied_request(reference.request.type.name());
    auto equivalent = reference;
    equivalent.name = CommandName(copied_name);
    equivalent.request.type = cxx::TypeToken(reference.request.type.hash(), copied_request);
    CHECK(compatibleCommands(reference, equivalent));
    for (unsigned fault = 0; fault < 10; ++fault)
    {
        auto changed = reference;
        if (fault == 0) changed.name = CommandName("test.Other");
        if (fault == 1) ++changed.version.minor;
        if (fault == 2) ++changed.request.size;
        if (fault == 3) changed.request.alignment *= 2;
        if (fault == 4) changed.response.type = cxx::typeToken<Ack>();
        if (fault == 5) changed.request.type = cxx::TypeToken(changed.request.type.hash(), "same-hash-other-name");
        if (fault == 6) changed.request.type = cxx::TypeToken(0, changed.request.type.name());
        if (fault == 7) changed.completion = ECompletionContract::RESOURCE_GPU_READY;
        if (fault == 8) ++changed.response.size;
        if (fault == 9) changed.response.alignment *= 2;
        auto mismatch = compatibleCommands(reference, changed);
        CHECK(!mismatch && mismatch.error().type == kProtocolContract);
    }
    for (unsigned fault = 0; fault < 7; ++fault)
    {
        auto changed = reference;
        if (fault == 0) changed.name = {};
        if (fault == 1) changed.version.major = 0;
        if (fault == 2) changed.request.type = {};
        if (fault == 3) changed.request.size = 0;
        if (fault == 4) changed.response.alignment = 0;
        if (fault == 5) changed.response.alignment = 3;
        if (fault == 6) changed.completion = static_cast<ECompletionContract>(255);
        CHECK(!validateCommand(changed));
    }
    CHECK(*CommandValueTraits<Query::Request>::retainedBytes({}) == sizeof(Query::Request));
    CHECK(*CommandValueTraits<Ack>::retainedBytes({}) == sizeof(Ack));
    return 0;
}

struct Completion final
{
    bool pending = true;
    bool gpu_ready = false;
    unsigned calls = 0;
    ECommandOutcome outcome = ECommandOutcome::OUTCOME_UNKNOWN;
    RenderError error{};
    Query::Response response;

    static RenderResult<void> finish(void* value, ECommandOutcome outcome, RenderError error, void* response) noexcept
    {
        auto& self = *static_cast<Completion*>(value);
        if (!self.pending || (self.gpu_ready && outcome == ECommandOutcome::SUCCESS))
            return cxx::unexpected(RenderError{kProtocolContract});
        if (response) self.response = std::move(*static_cast<Query::Response*>(response));
        self.outcome = outcome;
        self.error = error;
        self.pending = false;
        ++self.calls;
        return {};
    }

    static bool active(const void* value) noexcept { return static_cast<const Completion*>(value)->pending; }
};

static int context()
{
    static constexpr CommandCompletionFns functions{Completion::finish, Completion::active};
    Query::Request input{91};
    Completion completion;
    CommandInvocation invocation{&input, &completion, &functions};
    CommandContext<Query> context(invocation);
    CHECK(&context.request() == &input && context.request().key == 91 && context.active());
    Query::Response response{1, 2, 3};
    const auto* allocation = response.data();
    CHECK(context.complete(response));
    CHECK(!context.active() && completion.calls == 1 && completion.response.data() == allocation);
    Query::Response duplicate{4, 5};
    const auto* retained = duplicate.data();
    CHECK(!context.complete(duplicate) && duplicate.data() == retained && duplicate.size() == 2);
    CHECK(completion.calls == 1);

    completion = {};
    completion.gpu_ready = true;
    CHECK(!context.complete(duplicate) && context.active() && duplicate.data() == retained);
    CHECK(context.fail(RenderError{kProtocolContract, {17}}));
    CHECK(completion.outcome == ECommandOutcome::FAILED && completion.error.args[0] == 17);
    completion = {};
    CHECK(context.cancel());
    CHECK(completion.outcome == ECommandOutcome::CANCELLED && completion.calls == 1);
    completion = {};
    CHECK(context.complete(Query::Response{6, 7}));
    CHECK(completion.response == Query::Response({6, 7}));
    return 0;
}

int main()
{
    if (auto result = descriptor()) return result;
    return context();
}
