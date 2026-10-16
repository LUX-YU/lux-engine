#include <lux/engine/render/data/Payload.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <Windows.h>

using namespace lux::render;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); return 1; } } while (false)

static void require(bool value, const char* reason)
{
    if (!value)
    {
        std::fprintf(stderr, "Asset DLL: %s\n", reason);
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

static void sourceOwnerCode()
{
    // The last Owner ends through destruction, copy assignment and move
    // assignment. Each case must really unload an independently loaded DLL.
    for (unsigned release_path = 0; release_path != 3; ++release_path)
    {
        int destroyed = 0;
        int unpinned_destroyed = 0;
        bool pin_alive = true;
        const auto module = LoadLibraryW(L"render_data_payload_plugin.dll");
        require(module != nullptr, "plugin load failed");
        using Create = void (*)(std::shared_ptr<const std::vector<std::byte>>*, int*, int*, const bool*) noexcept;
        const auto create = reinterpret_cast<Create>(GetProcAddress(module, "payloadCreateAsset"));
        require(create != nullptr, "plugin Asset factory missing");
        auto pin = std::shared_ptr<const void>(module, [&pin_alive](const void* handle) noexcept {
            pin_alive = false;
            require(FreeLibrary(static_cast<HMODULE>(const_cast<void*>(handle))) != 0, "plugin unload failed");
        });
        std::shared_ptr<const std::vector<std::byte>> owner;
        create(&owner, &destroyed, &unpinned_destroyed, &pin_alive);
        const auto* original = owner->data();
        const auto capacity = owner->capacity();
        const auto references = owner.use_count();
        require(!RenderPayload::fromVector(owner, 31, 2, pin), "invalid source range accepted");
        require(owner.use_count() == references && owner->data() == original && pin_alive && destroyed == 0,
            "failed construction changed the original Owner or module qualification");
        auto payload = RenderPayload::fromVector(owner, 8, 4, pin);
        auto sibling = RenderPayload::fromVector(owner, 4, 3, pin);
        require(bool(payload) && bool(sibling), "valid source ranges rejected");
        require(payload->bytes().data() == original + 8 && sibling->bytes().data() == original + 4,
            "Payload must borrow both original Asset subranges without copying");
        require(payload->retainedBytes() == capacity && sibling->retainedBytes() == capacity,
            "every subrange must charge the complete original allocation");
        RenderPayload copied = *payload;
        RenderPayload moved = std::move(copied);
        require(copied.bytes().empty() && copied.bytes().data() == nullptr && copied.retainedBytes() == 0,
            "move construction left a source view behind");
        RenderPayload assigned;
        assigned = moved;
        auto final = std::make_unique<RenderPayload>();
        *final = std::move(assigned);
        require(assigned.bytes().empty() && assigned.retainedBytes() == 0,
            "move assignment left the previous source live");
        const auto& self_copy = *final;
        *final = self_copy;
        auto& self_move = *final;
        *final = std::move(self_move);
        require(final->bytes().data() == original + 8 && final->retainedBytes() == capacity,
            "self assignment lost ownership or the retained backing size");
        owner.reset();
        pin.reset();
        *payload = {};
        *sibling = {};
        moved = {};
        require(pin_alive && GetModuleHandleW(L"render_data_payload_plugin.dll") != nullptr && destroyed == 0,
            "the final Payload must pin the Asset deleter independently of Command code");
        require(final->bytes().view()[0] == std::byte{7}, "late source bytes became inaccessible");
        std::printf("Asset DLL: path=%u pin_alive=%d module_loaded=%d destroyed=%d; releasing final Asset owner\n",
            release_path, int(pin_alive),
            int(GetModuleHandleW(L"render_data_payload_plugin.dll") != nullptr), destroyed);
        std::fflush(stdout);
        const std::array replacement_bytes{std::byte{11}};
        auto replacement = RenderPayload::copyOf(replacement_bytes);
        if (release_path == 0) final.reset();
        else if (release_path == 1) *final = replacement;
        else *final = std::move(replacement);
        require(destroyed == 1 && unpinned_destroyed == 0,
            "Asset deleter must complete exactly once before the module is unpinned");
        require(!pin_alive && GetModuleHandleW(L"render_data_payload_plugin.dll") == nullptr,
            "the final Payload must release the module after the Owner is gone");
        if (final) require(final->bytes().view()[0] == std::byte{11}, "assignment lost the replacement Payload");
    }
    std::printf("Asset DLL: source code lifetime PASS, sizeof(RenderPayload)=%zu\n", sizeof(RenderPayload));
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 2 && std::string_view(argv[1]) == "source-owner")
    {
        sourceOwnerCode();
        return 0;
    }
    int releases = 0;
    auto owner = std::shared_ptr<std::vector<std::byte>>(
        new std::vector<std::byte>(32, std::byte{7}),
        [&releases](auto* value) noexcept { ++releases; delete value; }
    );
    owner->reserve(4096);
    const auto capacity = owner->capacity();
    const auto* source = owner->data();
    CHECK(!RenderPayload::fromVector({}, 0, 1));
    CHECK(!RenderPayload::fromVector(owner, 0, 0));
    CHECK(!RenderPayload::fromVector(owner, 33, 1));
    CHECK(!RenderPayload::fromVector(owner, 31, 2));
    CHECK(!RenderPayload::fromVector(owner, SIZE_MAX, 1));
    CHECK(!RenderPayload::fromVector(owner, 1, SIZE_MAX));
    auto value = RenderPayload::fromVector(owner, 8, 4);
    CHECK(value && value->retainedBytes() == capacity);
    CHECK(value->bytes().data() == source + 8 && value->bytes().size() == 4);
    owner.reset();
    CHECK(releases == 0 && value->bytes().view()[0] == std::byte{7});
    RenderPayload copied = *value;
    RenderPayload moved = std::move(copied);
    CHECK(copied.bytes().empty() && copied.bytes().data() == nullptr && copied.retainedBytes() == 0);
    *value = {};
    CHECK(releases == 0 && moved.bytes().data() == source + 8);
    CHECK(moved.retainedBytes() == capacity);
    moved = {};
    CHECK(releases == 1);

    std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
    auto configuration = RenderPayload::copyOf(bytes);
    CHECK(configuration.retainedBytes() == bytes.size());
    CHECK(configuration.bytes().data() != bytes.data());
    bytes[0] = std::byte{9};
    CHECK(configuration.bytes().view()[0] == std::byte{1});
    const auto empty = RenderPayload::copyOf({});
    CHECK(empty.retainedBytes() == 0 && empty.bytes().empty());
    sourceOwnerCode();
    return 0;
}
