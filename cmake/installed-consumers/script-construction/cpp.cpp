#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <array>
#include <cassert>
#include <limits>
#include <type_traits>

namespace
{
    using namespace lux::simulation::script;

    struct Probe final
    {
        void begin() noexcept {}

        ScriptCoroutine step(ScriptCoroutineContext&) noexcept
        {
            co_return;
        }
    };

    using Sync = detail::TCppStaticSyncEntry<&Probe::begin>;
    using Async = detail::TCppStaticCoroutineEntry<&Probe::step>;
    const std::array sync_exports{CppStaticExportEntry{1, "begin", Sync::Parameters, Sync::Results, &Sync::invoke}};
    const std::array async_exports{CppStaticExportEntry{1, "begin", Async::Parameters, {}, nullptr, &Async::start}};
    const CppStaticContract
        sync_contract{"lr01.sync", "lr01.sync", false, detail::cppStaticObject<Probe>(), sync_exports, {1, 0}};
    const CppStaticContract
        async_contract{"lr01.async", "lr01.async", false, detail::cppStaticObject<Probe>(), async_exports, {0, 0}};

    void rejected(std::span<const CppStaticScriptPoolDescription> pools)
    {
        const auto result = CppStaticScriptBackend::create(pools);
        assert(!result && result.error() == ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
    }
} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<CppStaticScriptBackend>);
    static_assert(std::is_nothrow_move_constructible_v<CppStaticScriptBackend>);
    const CppStaticScriptPoolDescription sync{&sync_contract, 2, 0, 0, alignof(std::max_align_t), 4};
    const CppStaticScriptPoolDescription
        async{&async_contract, 2, 2, 64 * 1024, alignof(std::max_align_t), 4, 512, true};
    rejected({});
    for (unsigned iteration{}; iteration != 32; ++iteration)
    {
        // Each second-pool rejection happens after the first pool has acquired its real slab.
        std::array pools{sync, async};
        pools[1].descriptor = nullptr;
        rejected(pools);
        pools[1] = sync;
        rejected(pools); // Duplicate canonical key.
        for (const auto bad_alignment : {0U, 3U})
        {
            pools[1] = async;
            pools[1].coroutine_frame_storage_alignment = bad_alignment;
            rejected(pools);
        }
        pools[1] = async;
        pools[1].coroutine_frame_storage_bytes = 1;
        rejected(pools);
        pools[1] = async;
        pools[1].coroutine_capacity = (std::numeric_limits<std::size_t>::max)();
        rejected(pools);
        pools[1] = async;
        pools[1].max_coroutine_frame_bytes = (std::numeric_limits<std::size_t>::max)();
        rejected(pools);
        pools[1] = async;
        pools[1].prepared_method_capacity = 0;
        rejected(pools);
        pools[1] = sync;
        pools[1].coroutine_capacity = 1;
        rejected(pools);

        pools[1] = async;
        auto first = CppStaticScriptBackend::create(pools);
        auto second = CppStaticScriptBackend::create(pools);
        assert(first && second && *first && *second);
        const auto before = first->stats();
        assert(before.frame_storage_bytes > 0 && before.active_frames == 0);
        assert(before.active_prepared_methods == 0 && before.active_artifact_associations == 0);
        *second = std::move(*first); // Old prepared slabs must be released before acquiring the new owner.
        assert(!*first && *second);
        assert(second->stats().frame_storage_bytes == before.frame_storage_bytes);
        CppStaticScriptBackend moved(std::move(*second));
        assert(moved && !*second);
        assert(moved.stats().frame_metadata_bytes == before.frame_metadata_bytes);
    }
}
