#include "AssetTest.hpp"

int main()
{
    using namespace asset_test;
    auto created = execution::ExecutionRuntime::create({1, 64, {32}, execution::BlockingSchedulerConfig{1, 32}});
    assert(created);
    auto execution = std::move(*created);
    asset::AssetVfs vfs;
    auto provider = std::make_shared<Provider>();
    provider->images.emplace(Id, image());
    provider->blocked = true;
    auto mount = vfs.mount({"/Game", provider});
    assert(mount);
    auto repository = AssetRepository::create(vfs.view(), {1024, 2048, 8}, {}, codecs());
    assert(repository);
    execution::TaskScope departing{*execution}, surviving{*execution};
    bool cancelled = false;
    AssetDataRef<std::thread::id> retained;
    assert(departing.spawn(stdexec::then(
        execution::asResult(read<asset::DecodeProbe>(**repository, Id)),
        [&](auto result) noexcept { cancelled = !result; }
    )));
    assert(surviving.spawn(stdexec::then(
        execution::asResult(read<asset::DecodeProbe>(**repository, Id)),
        [&](auto result) noexcept
        {
            assert(result);
            retained = std::move(*result);
        }
    )));
    assert(execution->waitUntil([&]() noexcept { return provider->entered.try_acquire(); }));
    departing.requestStop();
    // Accepted reads own a captured mount publication, even when the live mount is removed.
    *mount = {};
    provider->proceed.release();
    assert(execution->waitUntil([&]() noexcept { return cancelled && static_cast<bool>(retained); }));
    assert(provider->reads == 1);
    assert(wait<void>(*execution, departing.whenSettled()));
    assert(wait<void>(*execution, surviving.whenSettled()));
    retained = {};
    (*repository).reset();
    assert(!*repository);
    // A destroyed repository has no callable endpoint. Consumer cancellation was checked above.
    assert(departing.settled() && surviving.settled());
}
