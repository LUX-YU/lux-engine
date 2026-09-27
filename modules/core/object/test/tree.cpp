#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

#include <array>
#include <algorithm>
#include <cassert>
#include <memory>
#include <span>

namespace
{
    using namespace lux::object;

    struct Request final
    {
        int accept_at{};
        int filter_at{};
        bool close{};
        bool nested{};
    };

    struct Trace final
    {
        std::array<int, 32> entries{};
        std::size_t size{};

        void push(int value) noexcept
        {
            assert(size < entries.size());
            entries[size++] = value;
        }

        void expect(std::initializer_list<int> values) const
        {
            assert(size == values.size());
            assert(std::equal(values.begin(), values.end(), entries.begin()));
        }
    };

    class Node final : public lux::object::LuxObject
    {
    public:
        Node(ObjectDispatcherRef dispatcher, Trace& trace, int id) noexcept
            : lux::object::LuxObject(std::move(dispatcher)), trace_(trace), id_(id)
        {}

        Node(LuxObject& parent, Trace& trace, int id) noexcept : lux::object::LuxObject(&parent), trace_(trace), id_(id)
        {}

        ~Node() noexcept override
        {
            trace_.push(-id_);
        }
        bool close_requested{};
        static bool dispatching() noexcept
        {
            return isDispatching();
        }

    private:
        void filterEvent(LuxObject& target, EventView& view) noexcept override
        {
            assert(isDispatching());
            assert(&target != this);
            if (const auto* request = view.getIf<Request>())
            {
                trace_.push(id_ * 10);
                if (request->filter_at == id_)
                    view.accept();
            }
        }

        void event(EventView& view) noexcept override
        {
            assert(isDispatching());
            if (const auto* request = view.getIf<Request>())
            {
                trace_.push(id_);
                if (request->nested && id_ == 3)
                {
                    Request nested{.accept_at = 2};
                    assert(routeEvent(*this, *parent(), nested));
                }
                if (request->close)
                    close_requested = true;
                if (request->accept_at == id_)
                    view.accept();
            }
        }

        Trace& trace_;
        int id_;
    };

    void ownership()
    {
        auto messages_created = ObjectMessageQueue::create(64);
        assert(messages_created);
        auto messages = std::move(*messages_created);
        Trace trace;
        Node root(messages.dispatcherRef(), trace, 1);
        assert(!root.parent() && !root.firstChild() && !root.nextSibling());
        {
            Node first(root, trace, 2);
            auto middle = std::make_unique<Node>(root, trace, 3);
            Node last(root, trace, 4);
            assert(root.firstChild() == &first);
            assert(first.nextSibling() == middle.get());
            assert(middle->nextSibling() == &last);
            assert(!last.nextSibling());
            assert(first.dispatcherRef() == root.dispatcherRef());
            middle.reset();
            assert(first.nextSibling() == &last);
            trace.expect({-3});
        }
        assert(!root.firstChild());
        trace.expect({-3, -4, -2});
        Node replacement(root, trace, 5);
        assert(root.firstChild() == &replacement);
        assert(replacement.parent() == &root);
    }

    void routing()
    {
        Trace trace;
        Node root(ObjectDispatcherRef{}, trace, 1);
        Node middle(root, trace, 2);
        Node leaf(middle, trace, 3);

        Request request;
        assert(!Node::dispatching());
        assert(!sendEvent(leaf, request));
        assert(!Node::dispatching());
        trace.expect({3});
        trace.size = 0;
        assert(!routeEvent(leaf, root, request));
        assert(!Node::dispatching());
        trace.expect({10, 20, 3, 2, 1});

        trace.size = 0;
        request.accept_at = 2;
        assert(routeEvent(leaf, root, request));
        trace.expect({10, 20, 3, 2});

        trace.size = 0;
        request.filter_at = 1;
        assert(routeEvent(leaf, root, request));
        trace.expect({10});

        trace.size = 0;
        request.filter_at = 2;
        assert(routeEvent(leaf, root, request));
        trace.expect({10, 20});

        trace.size = 0;
        request = {.accept_at = 3};
        assert(routeEvent(leaf, leaf, request));
        trace.expect({3});

        trace.size = 0;
        request = {.accept_at = 3, .nested = true};
        assert(routeEvent(leaf, root, request));
        trace.expect({10, 20, 3, 20, 3, 2});

        trace.size = 0;
        request = {.accept_at = 3, .close = true};
        assert(routeEvent(leaf, root, request));
        assert(leaf.close_requested);
        assert(middle.firstChild() == &leaf);
        trace.expect({10, 20, 3});

        trace.size = 0;
        EventView accepted{request};
        accepted.accept();
        assert(routeEvent(leaf, root, accepted));
        trace.expect({});
        assert(!Node::dispatching());
    }

} // namespace

int main()
{
    ownership();
    routing();
}
