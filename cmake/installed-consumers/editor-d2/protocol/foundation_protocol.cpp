#include <cassert>
#include <cstdio>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <vector>

int main()
{
    using namespace lux;
    object::ObjectMessageQueue queue;
    const auto dispatcher = queue.dispatcherRef();
    std::vector<int> received;
    const auto post = [&](int value)
    {
        return object::detail::post(dispatcher,
                                    object::detail::makeMessage(
                                        [&, value]() noexcept
                                        {
                                            received.push_back(value);
                                            if (value == 1)
                                            {
                                                assert(object::detail::post(dispatcher, object::detail::makeMessage(
                                                                                            [&]() noexcept
                                                                                            {
                                                                                                received.push_back(4);
                                                                                            })) ==
                                                       object::detail::EPostStatus::POSTED);
                                            }
                                        }));
    };
    assert(post(1) == object::detail::EPostStatus::POSTED);
    assert(post(2) == object::detail::EPostStatus::POSTED);
    assert(post(3) == object::detail::EPostStatus::POSTED);
    assert(queue.dispatchPending(0) == 0 && received.empty());
    assert(queue.dispatchPending(2) == 2 && received == std::vector<int>({1, 2}));
    assert(queue.dispatchPending(1) == 1 && received.back() == 3);
    assert(queue.dispatchPending(64) == 1 && received.back() == 4);

    auto first = ui::UISession::create({}, dispatcher);
    auto second = ui::UISession::create({}, dispatcher);
    assert(first && second);
    assert(post(5) == object::detail::EPostStatus::POSTED);
    {
        auto frame = (*first)->beginFrame({{400, 300}, 1.0F / 60.0F, {1, 1}});
        frame.drawPanes();
        frame.finish();
    }
    assert(received.size() == 4);
    first->reset();
    assert(post(6) == object::detail::EPostStatus::POSTED);
    {
        auto frame = (*second)->beginFrame({{400, 300}, 1.0F / 60.0F, {1, 1}});
        frame.finish();
    }
    second->reset();
    assert(queue.dispatchPending(8) == 2 && received.back() == 6);
    auto owned = ui::UISession::create();
    assert(owned);
    const auto owned_dispatcher = (*owned)->dispatcherRef();
    bool dispatched{};
    assert(object::detail::post(owned_dispatcher, object::detail::makeMessage(
                                                      [&]() noexcept
                                                      {
                                                          dispatched = true;
                                                      })) == object::detail::EPostStatus::POSTED);
    {
        auto frame = (*owned)->beginFrame({{400, 300}, 1.0F / 60.0F, {1, 1}});
        frame.finish();
    }
    assert(dispatched);
    owned->reset();
    assert(object::detail::post(owned_dispatcher, object::detail::makeMessage(
                                                      []() noexcept
                                                      {
                                                      })) == object::detail::EPostStatus::CLOSED);
    queue.close();
    assert(post(7) == object::detail::EPostStatus::CLOSED);
    std::puts("PASS bounded FIFO/reentrant batch/borrowed UI/standalone UI/late closed dispatcher");
}
