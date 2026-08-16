//=============================================================================
// domain_events_test.cpp
// -----------------------------------------------------------------------------
// lux::events::DomainEvents 全行为覆盖(设计稿 .internal/event-system-design.md
// 附录 A;计划批 A 的验收清单):
//
//   * 发布/订阅/值送达;无订阅者 no-op;后订阅只见新事件
//   * 多泵各自送达;同泵多订阅者共享同一批次(地址同一性)
//   * 同通道 FIFO;drain 期发布留下轮;drainUntilEmpty 收敛 + 轮次上限
//   * 背压三策略(UNBOUNDED / DROP_NEWEST / DROP_OLDEST)+ dropped 计数
//     + diagnostics()
//   * Subscription RAII 退订/移动;handler 内退订自己;handler 内新订阅
//     (下一批才收)
//   * 主线程 owner 契约与无 replay 语义
//   * InlineRecord 打包/读回/截断
//
// bus 析构带存活订阅的 debug 断言不在此测(实测配置是 RelWithDebInfo,
// NDEBUG 下断言整条消失 —— CLAUDE.md「不要用 assert 表达实机契约」的
// 反面正是这里:这是纯编程契约,debug-only 是它的正确形态)。
//=============================================================================

#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/events/InlineRecord.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& desc)
{
    if (cond) { std::cout << "  PASS  " << desc << "\n"; ++g_pass; }
    else      { std::cout << "  FAIL  " << desc << "\n"; ++g_fail; }
}

static void banner(const char* title)
{
    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  " << title << "\n"
              << std::string(60, '=') << "\n";
}

// 事件类型定义在命名空间层(type_name 诊断可读;条例④:定义即注册)。
namespace ev
{
    struct Basic     { int v; };
    struct Order     { int v; };
    struct Multi     { int v; };
    struct Chain     { int remaining; };
    struct Infinite  { int v; };
    struct Unbounded { int v; };
    struct DropNew   { int v; };
    struct DropOld   { int v; };
    struct CfgLate   { int v; };
    struct Raii      { int v; };
    struct SelfUnsub { int v; };
    struct MidSub    { int v; };
    struct Emergency { int v; };
    struct Stress    { std::uint32_t producer; std::uint32_t seq; };
} // namespace ev

using lux::events::ChannelConfig;
using lux::events::EOverflow;
using lux::events::DomainEvents;
using lux::events::EventPump;
using lux::events::Subscription;
using lux::events::SubscriptionGroup;

int main()
{
    // 声明序 = 析构逆序:bus 最先声明、最后析构;订阅全部在它之后。
    DomainEvents   bus;
    EventPump& pump = bus.createPump("test");

    // ── 1. 发布/订阅/值送达 + 无订阅者 no-op ─────────────────────────
    banner("basic publish / subscribe / delivery");
    {
        bus.publish(ev::Basic{1});   // 无订阅者:丢弃,不崩

        int  calls  = 0;
        int  seen_v = 0;
        auto sub    = bus.subscribe<ev::Basic>(
            pump,
            [&](const ev::Basic& e) { ++calls; seen_v = e.v; });

        bus.publish(ev::Basic{2});
        pump.drain();
        check(calls == 1, "subscriber called exactly once");
        check(seen_v == 2, "event value delivered intact");
        check(calls == 1 && seen_v != 1,
              "publish before subscribe was a no-op (late subscriber sees "
              "only new events)");

        pump.drain();
        check(calls == 1, "second drain with empty queue delivers nothing");
    }

    // ── 2. 多泵各自送达 + 同泵共享批次 ───────────────────────────────
    banner("multi-pump delivery / same-pump shared batch");
    {
        EventPump& pump_b = bus.createPump("test-b");

        int         a1 = 0, a2 = 0, b1 = 0;
        const void* addr1 = nullptr;
        const void* addr2 = nullptr;

        auto s1 = bus.subscribe<ev::Multi>(
            pump, [&](const ev::Multi& e) { ++a1; addr1 = &e; });
        auto s2 = bus.subscribe<ev::Multi>(
            pump, [&](const ev::Multi& e) { ++a2; addr2 = &e; });
        auto s3 = bus.subscribe<ev::Multi>(
            pump_b, [&](const ev::Multi&) { ++b1; });

        bus.publish(ev::Multi{7});
        pump.drain();
        check(a1 == 1 && a2 == 1, "both subscribers on pump A delivered");
        check(addr1 == addr2,
              "same-pump subscribers share one batch (same const E& address)");
        check(b1 == 0, "pump B not drained yet — nothing delivered there");

        pump_b.drain();
        check(b1 == 1, "pump B delivers its own copy after its drain");
    }

    // ── 3. 同通道 FIFO ───────────────────────────────────────────────
    banner("per-channel FIFO");
    {
        std::vector<int> order;
        auto sub = bus.subscribe<ev::Order>(
            pump, [&](const ev::Order& e) { order.push_back(e.v); });

        for (int i = 0; i < 100; ++i)
            bus.publish(ev::Order{i});
        pump.drain();

        bool fifo = order.size() == 100;
        for (int i = 0; fifo && i < 100; ++i)
            if (order[static_cast<std::size_t>(i)] != i)
                fifo = false;
        check(fifo, "100 events delivered in publish order");
    }

    // ── 4. drain 期发布留下轮 / drainUntilEmpty 收敛 / 轮次上限 ──────
    banner("publish-during-drain stays for next round / drainUntilEmpty");
    {
        int  chain_calls = 0;
        auto sub         = bus.subscribe<ev::Chain>(
            pump,
            [&](const ev::Chain& e)
            {
                ++chain_calls;
                if (e.remaining > 0)
                    bus.publish(ev::Chain{e.remaining - 1});
            });

        bus.publish(ev::Chain{9});
        pump.drain();
        check(chain_calls == 1,
              "handler-published event NOT delivered in the same drain");

        pump.drainUntilEmpty();
        check(chain_calls == 10,
              "drainUntilEmpty converges the 9-deep republish chain");

        int  inf_calls = 0;
        auto inf       = bus.subscribe<ev::Infinite>(
            pump,
            [&](const ev::Infinite&)
            {
                ++inf_calls;
                bus.publish(ev::Infinite{0});
            });
        bus.publish(ev::Infinite{0});
        pump.drainUntilEmpty(8);
        check(inf_calls == 8,
              "self-feeding chain stopped by the max_rounds cap");
        // 收尾:退订后把残留的一条排掉,不污染后续段落。
        inf.reset();
        pump.drain();
    }

    // ── 5. 背压三策略 + dropped 计数 + diagnostics ───────────────────
    banner("backpressure: UNBOUNDED / DROP_NEWEST / DROP_OLDEST");
    {
        int  unbounded_calls = 0;
        auto su              = bus.subscribe<ev::Unbounded>(
            pump, [&](const ev::Unbounded&) { ++unbounded_calls; });
        for (int i = 0; i < 10000; ++i)
            bus.publish(ev::Unbounded{i});
        pump.drain();
        check(unbounded_calls == 10000, "UNBOUNDED: 10k events, zero loss");

        // DROP_NEWEST:生产端软限弃投 —— 只有前 capacity 条进队。
        bus.configure<ev::DropNew>(ChannelConfig{4, EOverflow::DROP_NEWEST});
        std::vector<int> got_new;
        auto sn = bus.subscribe<ev::DropNew>(
            pump, [&](const ev::DropNew& e) { got_new.push_back(e.v); });
        for (int i = 0; i < 10; ++i)
            bus.publish(ev::DropNew{i});
        pump.drain();
        check(got_new.size() == 4, "DROP_NEWEST: capacity 4 of 10 delivered");
        check(got_new == std::vector<int>({0, 1, 2, 3}),
              "DROP_NEWEST keeps the OLDEST four (first is most valuable)");

        // DROP_OLDEST:drain 端超量丢头部 —— 只有最后 capacity 条送达。
        bus.configure<ev::DropOld>(ChannelConfig{4, EOverflow::DROP_OLDEST});
        std::vector<int> got_old;
        auto so = bus.subscribe<ev::DropOld>(
            pump, [&](const ev::DropOld& e) { got_old.push_back(e.v); });
        for (int i = 0; i < 10; ++i)
            bus.publish(ev::DropOld{i});
        pump.drain();
        check(got_old.size() == 4, "DROP_OLDEST: capacity 4 of 10 delivered");
        check(got_old == std::vector<int>({6, 7, 8, 9}),
              "DROP_OLDEST keeps the NEWEST four (latest is the truth)");

        // configure 在订阅之后也生效(已有 PerPump 同步更新)。
        int  late_calls = 0;
        auto sc         = bus.subscribe<ev::CfgLate>(
            pump, [&](const ev::CfgLate&) { ++late_calls; });
        bus.configure<ev::CfgLate>(ChannelConfig{2, EOverflow::DROP_NEWEST});
        for (int i = 0; i < 10; ++i)
            bus.publish(ev::CfgLate{i});
        pump.drain();
        check(late_calls == 2, "configure after subscribe applies to the "
                               "existing per-pump queue");

        // diagnostics:弃投计数可见(「会静默丢事件的通道正是要消灭的」)。
        std::uint64_t drop_new_count = 0, drop_old_count = 0, stress_seen = 0;
        for (const auto& d : bus.diagnostics())
        {
            if (d.event_type.find("DropNew") != std::string_view::npos)
                drop_new_count += d.dropped;
            if (d.event_type.find("DropOld") != std::string_view::npos)
                drop_old_count += d.dropped;
            (void)stress_seen;
        }
        check(drop_new_count == 6, "diagnostics: DROP_NEWEST dropped == 6");
        check(drop_old_count == 6, "diagnostics: DROP_OLDEST dropped == 6");
    }

    // ── 6. Subscription RAII / 移动 / handler 内退订自己 ─────────────
    banner("subscription RAII / move / self-unsubscribe in handler");
    {
        int calls = 0;
        {
            auto sub = bus.subscribe<ev::Raii>(
                pump, [&](const ev::Raii&) { ++calls; });
            bus.publish(ev::Raii{1});
            pump.drain();
            check(calls == 1, "RAII: delivered while subscription alive");
        }
        bus.publish(ev::Raii{2});
        pump.drain();
        check(calls == 1, "RAII: scope exit unsubscribed — no delivery");

        auto sub_a = bus.subscribe<ev::Raii>(
            pump, [&](const ev::Raii&) { ++calls; });
        Subscription sub_b = std::move(sub_a);
        check(!sub_a.active() && sub_b.active(),
              "move: source inactive, target active");
        bus.publish(ev::Raii{3});
        pump.drain();
        check(calls == 2, "move: moved-to subscription still delivers");
        sub_b.reset();
        bus.publish(ev::Raii{4});
        pump.drain();
        check(calls == 2, "explicit reset() unsubscribes");

        // handler 内退订自己:同批后续事件不再送它;旁观者不受影响。
        int          self_calls = 0, witness_calls = 0;
        Subscription self;
        self = bus.subscribe<ev::SelfUnsub>(
            pump,
            [&](const ev::SelfUnsub&)
            {
                ++self_calls;
                self.reset();
            });
        auto witness = bus.subscribe<ev::SelfUnsub>(
            pump, [&](const ev::SelfUnsub&) { ++witness_calls; });
        for (int i = 0; i < 3; ++i)
            bus.publish(ev::SelfUnsub{i});
        pump.drain();
        check(self_calls == 1,
              "self-unsubscribe: handler stops mid-batch after reset()");
        check(witness_calls == 3, "self-unsubscribe: witness unaffected");
    }

    // ── 7. handler 内新订阅:下一批才收 ──────────────────────────────
    banner("subscribe during drain: delivery starts next batch");
    {
        int          first_calls = 0, late_calls = 0;
        Subscription late;
        auto         first = bus.subscribe<ev::MidSub>(
            pump,
            [&](const ev::MidSub&)
            {
                ++first_calls;
                if (!late.active())
                    late = bus.subscribe<ev::MidSub>(
                        pump, [&](const ev::MidSub&) { ++late_calls; });
            });

        bus.publish(ev::MidSub{1});
        bus.publish(ev::MidSub{2});
        pump.drain();
        check(first_calls == 2 && late_calls == 0,
              "mid-drain subscriber does NOT receive the current batch");
        bus.publish(ev::MidSub{3});
        pump.drain();
        check(late_calls == 1, "mid-drain subscriber receives the next batch");
    }

    // ── 8. owner thread / no replay ───────────────────────────────────
    banner("main-thread ownership / no replay");
    {
        check(bus.isOwnerThread(), "DomainEvents is owned by the constructing thread");
        bus.publish(ev::Emergency{1});
        int calls = 0;
        auto sub = bus.subscribe<ev::Emergency>(
            pump,
            [&](const ev::Emergency&) { ++calls; });
        pump.drain();
        check(calls == 0, "facts are not replayed to a late subscriber");
        bus.publish(ev::Emergency{2});
        pump.drain();
        check(calls == 1, "committed facts publish on the owner thread");
    }

    // ── 9. InlineRecord 打包/读回/截断 ───────────────────────────────
    banner("InlineRecord pack / unpack / truncation");
    {
        lux::events::InlineRecord<64> rec;
        check(rec.pack(42) && rec.pack(3.5) && rec.packString("hello"),
              "pack int + double + string into 64 bytes");
        check(!rec.truncated(), "no truncation within capacity");

        lux::events::InlineRecordReader rd(rec);
        int              i{};
        double           d{};
        std::string_view s;
        check(rd.unpack(i) && i == 42, "unpack int roundtrip");
        check(rd.unpack(d) && d == 3.5, "unpack double roundtrip");
        check(rd.unpackString(s) && s == "hello", "unpack string roundtrip");
        check(rd.remaining() == 0, "reader consumed exactly what was written");

        lux::events::InlineRecord<16> small;
        check(small.pack(1.0) && small.pack(2.0), "16-byte record takes two doubles");
        check(!small.pack(3.0), "third double refused");
        check(small.truncated(), "overflow sets the truncated flag");

        lux::events::InlineRecord<8> tiny;
        check(!tiny.packString("abcdefghij"),
              "oversize string reports truncation");
        check(tiny.truncated(), "truncated flag set");
        lux::events::InlineRecordReader rd2(tiny);
        std::string_view partial;
        check(rd2.unpackString(partial) && partial == "abcdef",
              "length prefix stays consistent — reader sees the kept prefix");
    }

    std::cout << "\n" << std::string(60, '-') << "\n"
              << "  total: " << g_pass << " passed, " << g_fail << " failed\n"
              << std::string(60, '-') << "\n";
    return g_fail == 0 ? 0 : 1;
}
