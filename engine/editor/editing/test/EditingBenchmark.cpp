#include "EditingFixtures.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>

using namespace lux::editor::editing::test;
namespace
{
    using Clock = std::chrono::steady_clock;
    template <class F> std::int64_t time(F action)
    {
        const auto start = Clock::now();
        action();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    }
    template <class S, class Edit, class Verify>
    void measure(const char* name, S& session, std::size_t count, Edit edit, Verify verify)
    {
        const auto execute_ns = time(
            [&]
            {
                for (std::size_t i = 0U; i < count; ++i)
                {
                    edit(i);
                }
            }
        );
        const auto retained = session.history->view()->snapshot;
        const auto checksum = verify();
        const auto undo_ns = time(
            [&]
            {
                for (std::size_t i = 0U; i < count; ++i)
                {
                    assert(session.history->undo());
                }
            }
        );
        assert(verify() == checksum);
        const auto redo_ns = time(
            [&]
            {
                for (std::size_t i = 0U; i < count; ++i)
                {
                    assert(session.history->redo());
                }
            }
        );
        assert(verify() == checksum);
        const auto clear_ns = time([&] { assert(session.history->clear()); });
        const auto close_ns = time([&] { assert(session.history->close()); });
        assert(session.stats.applies == count * 3U && session.stats.notices == count * 3U + 2U);
        assert(session.stats.operations == count && session.stats.operations_destroyed == count);
        assert(session.stats.plans == count * 3U && session.stats.plans_destroyed == count * 3U);
        std::cout << name << ',' << count << ',' << execute_ns << ',' << undo_ns << ',' << redo_ns << ',' << clear_ns
                  << ',' << close_ns << ',' << checksum << ',' << session.stats.applies << ',' << session.stats.notices
                  << ',' << session.stats.operations_destroyed << ',' << session.stats.plans_destroyed << ','
                  << retained.history_metadata_bytes << ',' << retained.charged_retained_bytes << ','
                  << session.stats.staging_used << '\n';
    }
    void text(std::size_t count)
    {
        TextSession s(std::string(256U, 'a'));
        measure(
            "text256",
            s,
            count,
            [&](std::size_t i)
            {
                execute(
                    s, s.replace(0U, std::string(1U, s.text()[0U]), std::string(1U, s.text()[0U] == 'a' ? 'b' : 'a'))
                );
            },
            [&]
            {
                // Each measured batch uses a fixed position so even workloads finish at the initial content.
                assert(s.text() == std::string(256U, 'a'));
                std::uint64_t sum{};
                for (const auto ch : s.text())
                {
                    sum += static_cast<unsigned char>(ch);
                }
                return sum;
            }
        );
    }
    void records(std::size_t count, int size, bool structural)
    {
        RecordSession s;
        for (int key = 1; key <= size; ++key)
        {
            execute(s, s.patch(key, {}, key * 3));
        }
        assert(s.history->clear());
        s.stats = {};
        const auto name = structural ? (size == 64 ? "create_delete64" : "create_delete1024") : "records64";
        measure(
            name,
            s,
            count,
            [&](std::size_t i)
            {
                if (structural)
                {
                    execute(s, i % 2U == 0U ? s.patch(size + 1, {}, 17) : s.patch(size + 1, 17, {}));
                }
                else
                {
                    execute(s, s.patch(1, i % 2U == 0U ? 3 : 4, i % 2U == 0U ? 4 : 3));
                }
            },
            [&]
            {
                assert(s.records().size() == static_cast<std::size_t>(size));
                std::uint64_t sum{};
                for (const auto& [key, value] : s.records())
                {
                    assert(value == key * 3);
                    sum += value;
                }
                assert(sum == static_cast<std::uint64_t>(size) * (size + 1U) * 3U / 2U);
                return sum;
            }
        );
    }
} // namespace
int main(int argc, char** argv)
{
    const auto count = argc == 2 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 1000U;
    assert(count == 1000U || count == 10000U);
    {
        TextSession warm;
        for (int i = 0; i < 100; ++i)
        {
            execute(warm, warm.replace(0U, warm.text(), i % 2 == 0 ? "beta" : "alpha"));
        }
        for (int i = 0; i < 100; ++i)
        {
            assert(warm.history->undo());
        }
        for (int i = 0; i < 100; ++i)
        {
            assert(warm.history->redo());
        }
    }
    {
        RecordSession warm;
        for (int i = 0; i < 100; ++i)
        {
            execute(warm, i % 2 == 0 ? warm.patch(1, {}, 7) : warm.patch(1, 7, {}));
        }
        for (int i = 0; i < 100; ++i)
        {
            assert(warm.history->undo());
        }
        for (int i = 0; i < 100; ++i)
        {
            assert(warm.history->redo());
        }
    }
    std::cout
        << "business,count,execute_ns,undo_ns,redo_ns,clear_ns,close_ns,checksum,applies,notices,operations_destroyed,"
           "plans_destroyed,metadata_bytes,retained_bytes,staging_bytes\n";
    text(count);
    records(count, 64, false);
    records(1000U, 64, true);
    records(1000U, 1024, true);
}
