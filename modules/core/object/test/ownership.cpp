#include <cassert>
#include <cstdio>
#include <functional>
#include <optional>
#include <string_view>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <thread>
#include <vector>

using namespace lux::object;
static_assert(!std::is_copy_constructible_v<ObjectDeleter> && !std::is_copy_assignable_v<ObjectDeleter>);
static_assert(std::is_nothrow_move_constructible_v<ObjectDeleter> && std::is_nothrow_move_assignable_v<ObjectDeleter>);
namespace
{
    class Node : public LuxObject
    {
    public:
        using LuxObject::adoptChild;
        using LuxObject::attachChild;
        using LuxObject::beginTreeVisit;
        using LuxObject::clearChildren;
        using LuxObject::detachChild;
        using LuxObject::emit;
        using LuxObject::endTreeVisit;
        using LuxObject::LuxObject;
        std::function<void()> cleanup;
        TSignal<> changed{*this};
        ~Node() override
        {
            if (cleanup)
            {
                cleanup();
            }
        }
    };
    struct Deleter final
    {
        int* releases{};
        int* moves{};
        Deleter(int& released, int& moved) noexcept : releases(&released), moves(&moved) {}
        Deleter(Deleter&& other) noexcept : releases(std::exchange(other.releases, nullptr)), moves(other.moves)
        {
            ++*moves;
        }
        Deleter(const Deleter&) = delete;
        void operator()(Node* node) noexcept
        {
            ++*releases;
            delete node;
        }
    };

    void mixedTree()
    {
        Node survivor;
        std::vector<int> order;
        {
            Node parent;
            Node member;
            assert(parent.attachChild(member));
            assert(parent.attachChild(survivor));
            int releases{}, moves{};
            auto first = std::unique_ptr<Node, Deleter>(new Node, Deleter{releases, moves});
            first->cleanup = [&] { order.push_back(1); };
            auto adopted = parent.adoptChild(std::move(first));
            assert(adopted && !first);
            assert((*adopted)->ownership() == EObjectOwnership::PARENT_OWNED);
            assert(!parent.detachChild(**adopted));
            auto second = std::make_unique<Node>();
            second->cleanup = [&] { order.push_back(2); };
            assert(parent.adoptChild(std::move(second)));
            // Explicit pre-cleanup while borrowed derived members are still alive.
            parent.clearChildren();
            assert(releases == 1 && order == std::vector<int>({2, 1}));
            assert(!member.parent() && !survivor.parent());
            assert(!parent.firstChild());
        }
        auto parent = std::make_unique<Node>();
        assert(parent->attachChild(survivor));
        auto dynamic = std::make_unique<Node>();
        dynamic->cleanup = [&] { order.push_back(3); };
        assert(parent->adoptChild(std::move(dynamic)));
        parent.reset();
        assert(!survivor.parent() && order.back() == 3);
    }

    void refusalAndCleanup()
    {
        Node parent, other;
        int releases{}, moves{};
        auto child = std::unique_ptr<Node, Deleter>(new Node, Deleter{releases, moves});
        assert(other.attachChild(*child));
        const auto before = moves;
        auto code_owner = std::make_shared<int>(1);
        std::weak_ptr<int> code_weak = code_owner;
        auto code = CodeLease::plugin(std::move(code_owner));
        auto rejected = parent.adoptChild(std::move(child), std::move(code));
        assert(!rejected && rejected.error() == EObjectTreeError::ALREADY_ATTACHED);
        assert(child && moves == before && !releases && child->parent() == &other);
        assert(code.valid() && !code_weak.expired()); // Rejection does not consume the caller's last code pin.
        assert(other.detachChild(*child));
        parent.beginTreeVisit();
        rejected = parent.adoptChild(std::move(child));
        assert(!rejected && rejected.error() == EObjectTreeError::BUSY && child && moves == before);
        parent.endTreeVisit();
        std::thread wrong(
            [&]
            {
                auto result = parent.adoptChild(std::move(child));
                assert(!result && result.error() == EObjectTreeError::WRONG_THREAD);
            }
        );
        wrong.join();
        assert(child && moves == before);
        Node callback_target;
        int callbacks{};
        auto connection = LuxObject::connect(&other, &Node::changed, &parent, [&]() noexcept { ++callbacks; });
        assert(connection);
        child->cleanup = [&]
        {
            assert(!parent.attachChild(callback_target));
            auto late = LuxObject::connect(&other, &Node::changed, &parent, []() noexcept {});
            assert(!late && late.error() == EConnectError::OBJECT_CLOSED);
            (void)other.emit(other.changed);
        };
        assert(parent.adoptChild(std::move(child)));
        parent.clearChildren();
        assert(!callbacks && !callback_target.parent() && releases == 1);
        assert(parent.attachChild(callback_target));
        assert(!callback_target.attachChild(parent));
    }

    void retirement()
    {
        auto created = ObjectMessageQueue::create(1);
        assert(created);
        auto queue = std::move(*created);
        Node parent(queue.dispatcherRef()), sender(queue.dispatcherRef());
        int destroyed{};
        auto child = std::make_unique<Node>(queue.dispatcherRef());
        child->cleanup = [&] { ++destroyed; };
        auto* pointer = child.get();
        assert(parent.adoptChild(std::move(child)));
        auto connection = LuxObject::connect(
            &sender,
            &Node::changed,
            pointer,
            [&]() noexcept
            {
                assert(pointer->requestDestruction());
                assert(pointer->requestDestruction());
                assert(queue.collectRetired() == 0 && !destroyed);
            }
        );
        assert(connection);
        (void)sender.emit(sender.changed);
        assert(queue.pendingRetirements() == 1 && !destroyed);
        assert(queue.collectRetired() == 1 && destroyed == 1);
        assert(!parent.firstChild() && !queue.pendingRetirements());
        assert(!parent.requestDestruction());

        auto late = std::make_unique<Node>(queue.dispatcherRef());
        auto late_ptr = parent.adoptChild(std::move(late));
        assert(late_ptr && (*late_ptr)->requestDestruction());
        parent.clearChildren(); // Already queued identity becomes stale, never dereferences its old address.
        assert(queue.collectRetired() == 1 && !queue.pendingRetirements());

        auto shared_candidate = std::make_unique<Node>(queue.dispatcherRef());
        const auto affinity = std::this_thread::get_id();
        shared_candidate->cleanup = [&]
        {
            assert(std::this_thread::get_id() == affinity);
            ++destroyed;
        };
        auto shared = shareOnDispatcher(queue.dispatcherRef(), std::move(shared_candidate));
        assert(shared && !shared_candidate);
        std::weak_ptr<Node> weak = *shared;
        std::thread worker([owner = std::move(*shared)]() mutable { owner.reset(); });
        worker.join();
        assert(weak.expired() && destroyed == 1 && queue.pendingRetirements() == 1);
        queue.close(); // Message admission can close before retirement responsibilities drain.
        assert(queue.collectRetired() == 1 && destroyed == 2);
        weak.reset();
    }

    void fixedBatchAndIdentity()
    {
        auto queue = ObjectMessageQueue::create(1);
        assert(queue);
        Node parent(queue->dispatcherRef()), other(queue->dispatcherRef());
        int destroyed{};
        auto second = std::make_unique<Node>(queue->dispatcherRef());
        second->cleanup = [&] { ++destroyed; };
        auto second_ptr = other.adoptChild(std::move(second));
        assert(second_ptr);
        auto first = std::make_unique<Node>(queue->dispatcherRef());
        first->cleanup = [&]
        {
            assert((*second_ptr)->requestDestruction());
            assert(queue->collectRetired() == 0); // Never recursively drain a deleter's new request.
        };
        auto first_ptr = parent.adoptChild(std::move(first));
        assert(first_ptr && (*first_ptr)->requestDestruction());
        assert(queue->collectRetired() == 1 && !destroyed && queue->pendingRetirements() == 1);
        assert(queue->collectRetired() == 1 && destroyed == 1);

        alignas(Node) std::byte storage[sizeof(Node)];
        auto destroy = [](Node* node) noexcept { std::destroy_at(node); };
        using PlacementOwner = std::unique_ptr<Node, decltype(destroy)>;
        PlacementOwner old{std::construct_at(reinterpret_cast<Node*>(storage), queue->dispatcherRef()), destroy};
        auto old_ptr = parent.adoptChild(std::move(old));
        assert(old_ptr && (*old_ptr)->requestDestruction());
        parent.clearChildren();
        PlacementOwner replacement{
            std::construct_at(reinterpret_cast<Node*>(storage), queue->dispatcherRef()),
            destroy
        };
        auto* address = replacement.get();
        replacement->cleanup = [&] { ++destroyed; };
        assert(parent.adoptChild(std::move(replacement)));
        assert(queue->collectRetired() == 1 && destroyed == 1 && parent.firstChild() == address);
        parent.clearChildren();
        assert(destroyed == 2);

        auto plain = std::make_unique<int>(42);
        auto shared = shareOnDispatcher(queue->dispatcherRef(), std::move(plain));
        assert(shared && **shared == 42 && !plain);
        shared->reset();
        assert(queue->collectRetired() == 1);
    }

    void partialConstructionAndDerivedResource()
    {
        struct Composite final : Node
        {
            bool resource_alive{true};
            ~Composite() override
            {
                clearChildren();
                resource_alive = false;
            }
        };
        int destroyed{};
        auto owner = std::make_unique<Composite>();
        auto first = std::make_unique<Node>();
        first->cleanup = [&]
        {
            assert(owner->resource_alive);
            ++destroyed;
        };
        auto* first_ptr = first.get();
        assert(owner->adoptChild(std::move(first)));
        auto factory = [&]() -> ObjectResult<std::unique_ptr<Node>>
        {
            auto candidate = std::make_unique<Node>();
            candidate->cleanup = [&] { ++destroyed; };
            return lux::cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
        };
        auto rejected = factory();
        assert(!rejected && destroyed == 1 && owner->firstChild() == first_ptr);
        // Keep an independent borrow: unique_ptr::reset replaces its stored pointer before deleting.
        auto* borrow = owner.get();
        first_ptr->cleanup = [&, borrow]
        {
            assert(borrow->resource_alive);
            ++destroyed;
        };
        owner.reset();
        assert(destroyed == 2);
    }

    void queuedRemovalAndShapes()
    {
        auto queue = ObjectMessageQueue::create(1);
        assert(queue);
        Node owner(queue->dispatcherRef()), sender(queue->dispatcherRef());
        int destroyed{};
        auto child = std::make_unique<Node>(queue->dispatcherRef());
        child->cleanup = [&] { ++destroyed; };
        auto adopted = owner.adoptChild(std::move(child));
        assert(adopted);
        auto connection = LuxObject::connect(
            &sender,
            &Node::changed,
            *adopted,
            [&]() noexcept
            {
                assert((*adopted)->requestDestruction());
                assert(queue->collectRetired() == 0 && !destroyed);
            },
            EDelivery::QUEUED
        );
        assert(connection);
        assert(sender.emit(sender.changed).queued == 1);
        assert(sender.emit(sender.changed).full == 1);
        assert(queue->dispatchPending() == 1 && !destroyed);
        assert(queue->collectRetired() == 1 && destroyed == 1);

        for (int round = 0; round != 8; ++round)
        {
            Node tree;
            int count{};
            auto* parent = &tree;
            for (int depth = 0; depth != 1024; ++depth)
            {
                auto next = std::make_unique<Node>();
                next->cleanup = [&] { ++count; };
                auto result = parent->adoptChild(std::move(next));
                assert(result);
                parent = *result;
            }
            tree.clearChildren();
            assert(count == 1024 && !tree.firstChild());
            for (int width = 0; width != 2048; ++width)
            {
                auto next = std::make_unique<Node>();
                next->cleanup = [&] { ++count; };
                assert(tree.adoptChild(std::move(next)));
            }
            tree.clearChildren();
            assert(count == 3072 && !tree.firstChild());
        }
    }

    void sharingDeleterReentry()
    {
        struct ReentrantDeleter final
        {
            Node* parent{};
            Node** target{};
            int* rejected{};
            ReentrantDeleter(Node& owner, Node*& value, int& count) noexcept
                : parent(&owner), target(&value), rejected(&count)
            {
            }
            ReentrantDeleter(ReentrantDeleter&& other) noexcept
                : parent(other.parent), target(other.target), rejected(other.rejected)
            {
                if (*target)
                {
                    assert((*target)->ownership() == EObjectOwnership::EXTERNAL && !(*target)->parent());
                    auto result = parent->attachChild(**target);
                    assert(!result && result.error() == EObjectTreeError::BUSY);
                    ++*rejected;
                }
            }
            void operator()(Node* value) noexcept
            {
                delete value;
            }
        };
        auto queue = ObjectMessageQueue::create(1);
        assert(queue);
        Node parent(queue->dispatcherRef());
        Node* target{};
        int rejected{};
        auto candidate = std::unique_ptr<Node, ReentrantDeleter>(
            new Node(queue->dispatcherRef()),
            ReentrantDeleter{parent, target, rejected}
        );
        target = candidate.get();
        auto result = shareOnDispatcher(queue->dispatcherRef(), std::move(candidate));
        assert(result && rejected > 0 && !candidate && !target->parent());
        assert(parent.attachChild(*target)); // Guard is released only after the owning control block exists.
        result->reset();
        assert(queue->collectRetired() == 1 && !parent.firstChild());
        target = nullptr;
        auto adopted_candidate = std::unique_ptr<Node, ReentrantDeleter>(
            new Node(queue->dispatcherRef()), ReentrantDeleter{parent, target, rejected}
        );
        target = adopted_candidate.get();
        auto adopted = parent.adoptChild(std::move(adopted_candidate));
        assert(adopted && !adopted_candidate && target->parent() == &parent);
        assert(target->ownership() == EObjectOwnership::PARENT_OWNED);
        parent.clearChildren();
    }

    void dispatcherRefusal()
    {
        auto first_queue = ObjectMessageQueue::create(1), second_queue = ObjectMessageQueue::create(1);
        assert(first_queue && second_queue);
        Node parent(first_queue->dispatcherRef());
        int releases{}, moves{};
        auto candidate =
            std::unique_ptr<Node, Deleter>(new Node(second_queue->dispatcherRef()), Deleter{releases, moves});
        const auto initial_moves = moves;
        auto* address = candidate.get();
        auto refused = parent.adoptChild(std::move(candidate));
        assert(!refused && refused.error() == EObjectTreeError::WRONG_DISPATCHER);
        auto shared = shareOnDispatcher(first_queue->dispatcherRef(), std::move(candidate));
        assert(!shared && shared.error() == EObjectTreeError::WRONG_DISPATCHER);
        assert(candidate.get() == address && moves == initial_moves && !releases && !address->parent());
        auto external = address->requestDestruction();
        assert(!external && external.error() == EObjectTreeError::NOT_OWNED);
    }

    void finalSafePoint()
    {
        int destroyed{}, delivered{};
        std::shared_ptr<Node> second;
        {
            auto queue = ObjectMessageQueue::create(4);
            assert(queue);
            Node sender(queue->dispatcherRef()), receiver(queue->dispatcherRef());
            auto connection = LuxObject::connect(
                &sender, &Node::changed, &receiver, [&]() noexcept { ++delivered; }, EDelivery::QUEUED
            );
            assert(connection && sender.emit(sender.changed).queued == 1);
            auto child = std::make_unique<Node>(queue->dispatcherRef());
            child->cleanup = [&] { ++destroyed; };
            auto shared = shareOnDispatcher(queue->dispatcherRef(), std::move(child));
            assert(shared);
            second = std::move(*shared);
            auto first = std::make_unique<Node>(queue->dispatcherRef());
            first->cleanup = [&] { second.reset(); ++destroyed; };
            auto owner = shareOnDispatcher(queue->dispatcherRef(), std::move(first));
            assert(owner);
            owner->reset();
            assert(!destroyed && queue->pendingRetirements() == 2);
        }
        assert(destroyed == 2 && delivered == 0 && !second);

        auto first = ObjectMessageQueue::create(1), replacement = ObjectMessageQueue::create(1);
        assert(first && replacement);
        auto candidate = std::make_unique<Node>(first->dispatcherRef());
        candidate->cleanup = [&] { ++destroyed; };
        auto owner = shareOnDispatcher(first->dispatcherRef(), std::move(candidate));
        assert(owner);
        owner->reset();
        *first = std::move(*replacement);
        assert(destroyed == 3 && first->dispatcherRef().isCurrent());
    }

    int rejectFinalSafePoint(std::string_view mode)
    {
        auto queue = ObjectMessageQueue::create(4);
        assert(queue);
        auto candidate = std::make_unique<Node>(queue->dispatcherRef());
        auto shared = shareOnDispatcher(queue->dispatcherRef(), std::move(candidate));
        assert(shared);
        std::optional<ObjectMessageQueue> provider{std::move(*queue)};
        std::puts("reached final safe-point contract");
        std::fflush(stdout);
        if (mode == "--reject-held")
            provider.reset();
        else if (mode == "--reject-dispatch")
        {
            Node sender(provider->dispatcherRef()), receiver(provider->dispatcherRef());
            auto connection = LuxObject::connect(&sender, &Node::changed, &receiver, [&]() noexcept {
                shared->reset();
                provider.reset();
            });
            assert(connection);
            (void)sender.emit(sender.changed);
        }
        else if (mode == "--reject-foreign")
        {
            shared->reset();
            std::thread worker([&] { provider.reset(); });
            worker.join();
        }
        return 0; // Reaching this line means the negative contract was silently weakened.
    }

    void pluginReplacement(const char* path)
    {
        using Library = lux::engine::platform::DynamicLibrary;
        using Owner = std::unique_ptr<LuxObject, ObjectDeleter>;
        using Make = void (*)(ObjectDispatcherRef, CodeLease, int*, Owner&) noexcept;
        int first_trace[4]{}, second_trace[4]{};
        auto load = [&](int* trace)
        {
            return std::shared_ptr<Library>(
                new Library(path),
                [trace](Library* value) noexcept
                {
                    assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1);
                    delete value;
                    ++trace[3];
                }
            );
        };
        auto first_library = load(first_trace), second_library = load(second_trace);
        assert(first_library->is_loaded() && second_library->is_loaded());
        auto create = reinterpret_cast<Make>(first_library->get_symbol("make_object"));
        assert(create);
        auto messages = ObjectMessageQueue::create(4);
        assert(messages);
        Owner first, second;
        create(messages->dispatcherRef(), CodeLease::plugin(first_library), first_trace, first);
        create(messages->dispatcherRef(), CodeLease::plugin(second_library), second_trace, second);
        Node attached(messages->dispatcherRef()), receiver(messages->dispatcherRef());
        assert(attached.attachChild(*first));
        auto* original = first.get();
        first_library.reset();
        second_library.reset();
        auto rejected = receiver.adoptChild(std::move(first));
        assert(!rejected && rejected.error() == EObjectTreeError::ALREADY_ATTACHED);
        assert(first.get() == original && first->parent() == &attached && !first_trace[0] && !first_trace[3]);
        assert(attached.detachChild(*first));
        first = std::move(second);
        assert(!second && first_trace[3] == 1 && !second_trace[0]);
        first.reset();
        assert(second_trace[0] == 1 && second_trace[1] == 1 && !second_trace[3]);
        first.get_deleter() = ObjectDeleter{};
        assert(second_trace[2] == 1 && second_trace[3] == 1);
    }

    void plugin(const char* path, bool parent_owned)
    {
        using Library = lux::engine::platform::DynamicLibrary;
        using Owner = std::unique_ptr<LuxObject, ObjectDeleter>;
        using Make = void (*)(ObjectDispatcherRef, CodeLease, int*, Owner&) noexcept;
        int trace[4]{};
        auto library = std::shared_ptr<Library>(
            new Library(path),
            [&](Library* value) noexcept
            {
                assert(trace[0] && trace[1] && trace[2]);
                delete value;
                ++trace[3];
            }
        );
        assert(library->is_loaded());
        auto create = reinterpret_cast<Make>(library->get_symbol("make_object"));
        assert(create);
        auto messages = ObjectMessageQueue::create(8);
        assert(messages);
        Owner candidate;
        create(messages->dispatcherRef(), CodeLease::plugin(library), trace, candidate);
        if (parent_owned)
        {
            Node parent(messages->dispatcherRef());
            assert(parent.adoptChild(std::move(candidate), CodeLease::plugin(library)));
            library.reset();
            assert(!trace[0] && !trace[3]);
            parent.clearChildren();
            assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1 && trace[3] == 1);
            return;
        }
        auto shared = shareOnDispatcher(messages->dispatcherRef(), std::move(candidate), CodeLease::plugin(library));
        assert(shared);
        std::weak_ptr<LuxObject> weak = *shared;
        library.reset();
        std::thread worker([owner = std::move(*shared)]() mutable { owner.reset(); });
        worker.join();
        assert(!trace[0] && !trace[3] && weak.expired());
        assert(messages->collectRetired() == 1);
        assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1 && trace[3] == 1);
        weak.reset(); // Host control block remains safe after the DLL has actually unloaded.
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]).starts_with("--reject-"))
        return rejectFinalSafePoint(argv[1]);
    mixedTree();
    refusalAndCleanup();
    retirement();
    fixedBatchAndIdentity();
    partialConstructionAndDerivedResource();
    queuedRemovalAndShapes();
    sharingDeleterReentry();
    dispatcherRefusal();
    finalSafePoint();
    if (argc == 2)
    {
        plugin(argv[1], true);
        plugin(argv[1], false);
        pluginReplacement(argv[1]);
    }
    std::puts("Object ownership: mixed tree, refusal, callback, retirement, worker release and DLL tail passed");
}
