#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Root.hpp>
#include <new>

// Implemented only by the real library's native-test build; not an installed production API.
extern "C" void luxEc3CommandCounts(std::uint64_t*, bool) noexcept;
namespace
{
    bool allocations_enabled{};
    std::size_t allocations{}, allocated_bytes{};
    void countAllocation(std::size_t size)
    {
        if (allocations_enabled)
        {
            ++allocations;
            allocated_bytes += size;
        }
    }
} // namespace
void* operator new(std::size_t size)
{
    countAllocation(size);
    if (auto* result = std::malloc(size ? size : 1))
    {
        return result;
    }
    std::abort();
}
void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete[](void* pointer) noexcept
{
    ::operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept
{
    ::operator delete(pointer);
}

using namespace lux::editor::commands;
using lux::object::CodeLease;
namespace
{
    constexpr CommandDescriptor fixed{CommandIdView{"measure.fixed"}, "Static command", "Measure"};
    auto query(const CommandQuery&) -> CommandResult<CommandState>
    {
        return CommandState{true};
    }
    auto execute(const CommandInvocation&) -> CommandResult<DispatchReceipt>
    {
        return DispatchReceipt{ImmediateCompletion{}};
    }
    using Counts = std::array<std::uint64_t, 9>;
    void begin()
    {
        Counts ignored{};
        luxEc3CommandCounts(ignored.data(), true);
        allocations = allocated_bytes = 0;
        allocations_enabled = true;
    }
    Counts finish(const char* label, std::size_t size)
    {
        allocations_enabled = false;
        Counts counts{};
        luxEc3CommandCounts(counts.data(), false);
        std::printf(
            "COUNT %s n=%zu allocations=%zu requested_bytes=%zu counts=",
            label,
            size,
            allocations,
            allocated_bytes
        );
        for (auto count : counts)
        {
            std::printf("%llu,", static_cast<unsigned long long>(count));
        }
        std::puts("");
        return counts;
    }
} // namespace
int main()
{
    std::printf(
        "SIZES descriptor=%zu entry=%zu handle=%zu; count scope=real static command library; "
        "caller ID constructors and DLL allocators excluded; timing counters disabled\n",
        sizeof(CommandDescriptor),
        sizeof(CommandEntry),
        sizeof(CommandHandle)
    );
    begin();
    auto builtin = CommandEntry::bind<fixed>(CodeLease::builtin(), query, execute);
    auto counts = finish("static", 1);
    assert(counts[5] == 0 && counts[6] == 0 && &builtin->descriptor() == &fixed);
    bool released{};
    std::weak_ptr<const void> weak;
    {
        auto owner = std::shared_ptr<const void>(
            new int{},
            [&](const void* p)
            {
                released = true;
                delete static_cast<const int*>(p);
            }
        );
        weak = owner;
        begin();
        auto plugin = CommandEntry::bind<fixed>(CodeLease::plugin(owner), query, execute);
        counts = finish("static-code-lease", 1);
        assert(counts[5] == 0 && counts[6] == 0);
        auto snapshot = CommandRegistrySnapshot::create({plugin});
        assert(snapshot);
        owner.reset();
        plugin.reset();
        assert(!released);
    }
    assert(released && weak.expired());
    std::puts("LEASE original owner released after last entry/snapshot; actual DLL unload covered by extension consumer"
    );
    for (std::size_t size : {35u, 256u, 1024u})
    {
        std::vector<std::shared_ptr<CommandEntry>> entries;
        entries.reserve(size);
        begin();
        for (std::size_t i{}; i < size; ++i)
        {
            const std::string name = "ec3.measure.extended.command." + std::to_string(i);
            entries.push_back(
                CommandEntry::create(CodeLease::builtin(), {CommandIdView{name}, name, "Measure"}, query, execute)
            );
        }
        counts = finish("dynamic-freeze", size);
        assert(counts[6] == size && counts[7] == size);
        begin();
        auto snapshot = CommandRegistrySnapshot::create(entries, size);
        assert(snapshot);
        counts = finish("register", size);
        assert(counts[0] == size && counts[4] >= size * 16);
        std::vector<CommandHandle> handles;
        handles.reserve(size);
        begin();
        for (const auto& entry : entries)
        {
            handles.push_back(*snapshot->find(entry->descriptor().id));
        }
        counts = finish("cold-find", size);
        assert(counts[0] == 0 && counts[1] == size && counts[2] == size);
        CommandRegistry registry;
        assert(registry.publish(*snapshot));
        {
            auto messages = lux::object::ObjectMessageQueue::create(32);
            assert(messages);
            auto root = lux::ui::Root::create(messages->dispatcherRef());
            assert(root);
            CommandDispatcher dispatcher{registry};
            lux::editor::desktop::CommandMenu menu{
                **root,
                registry,
                dispatcher,
                [](const CommandDescriptor&,
                   const lux::ui::Pane*,
                   const lux::ui::Element*) -> CommandResult<CommandInvocation> { return CommandInvocation{}; }
            };
            assert(menu.update());
            const auto* menu_data = (*root)->menu().data();
            begin();
            for (unsigned i{}; i < 1000; ++i)
            {
                assert(menu.update());
            }
            counts = finish("menu-stable-1000", size);
            assert((*root)->menu().data() == menu_data);
            assert(allocations == 0 && counts[0] == 0 && counts[1] == 0 && counts[8] == 0);
        }
        for (auto binding : {ERegistryBinding::PINNED, ERegistryBinding::CURRENT_REGISTRATION})
        {
            CommandDispatcher dispatcher{registry, 32};
            begin();
            for (unsigned i{}; i < 1000; ++i)
            {
                CommandInvocation input{{}, {}, binding};
                assert(dispatcher.enqueue(handles[i % size], input));
                auto result = dispatcher.drain();
                assert(result && result->size() == 1 && result->front().result);
            }
            counts = finish(binding == ERegistryBinding::PINNED ? "PINNED" : "CURRENT", size);
            assert(counts[0] == 0 && counts[1] == 0 && counts[5] == 0);
            assert(counts[2] == (binding == ERegistryBinding::PINNED ? 0u : 1000u));
        }
        for (unsigned sample{}; sample < 20; ++sample)
        {
            const auto start = std::chrono::steady_clock::now();
            auto candidate = CommandRegistrySnapshot::create(entries, size);
            const auto end = std::chrono::steady_clock::now();
            assert(candidate);
            std::printf(
                "TIME register n=%zu sample=%u ns=%lld\n",
                size,
                sample,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count())
            );
        }
        begin();
        auto replacement = CommandRegistrySnapshot::create(entries, size);
        finish("replacement", size);
        begin();
        for (const auto& handle : handles)
        {
            assert(replacement->resolve(handle));
        }
        counts = finish("CURRENT-same-entries-new-snapshot", size);
        assert(counts[0] == 0 && counts[1] == 0);
        std::vector<std::shared_ptr<CommandEntry>> replaced;
        for (const auto& entry : entries)
        {
            replaced.push_back(CommandEntry::create(CodeLease::builtin(), entry->descriptor(), query, execute));
        }
        auto changed = CommandRegistrySnapshot::create(std::move(replaced), size);
        assert(changed);
        begin();
        for (const auto& handle : handles)
        {
            assert(changed->resolve(handle));
        }
        counts = finish("CURRENT-replaced-entries", size);
        assert(counts[0] == 0 && counts[1] == size);
    }
}
