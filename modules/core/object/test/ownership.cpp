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
namespace
{
    template <class T>
    concept PinnableAllocation = requires(T owner) { pinCodeOwner(CodeLease::builtin(), std::move(owner)); };
    static_assert(PinnableAllocation<std::unique_ptr<int>>);
    static_assert(!PinnableAllocation<std::shared_ptr<int>>);

    class Node : public LuxObject
    {
    public:
        using LuxObject::beginTreeVisit;
        using LuxObject::clearChildren;
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

    void objectIdentities()
    {
        ObjectId empty;
        assert(!empty.isValid() && !ObjectRuntime::instance().resolve(empty) && ObjectRuntime::instance().resolve(empty).error() == EObjectTreeError::INVALID_OBJECT);
        alignas(Node) std::byte memory[sizeof(Node)];
        auto* first = ::new (memory) Node;
        auto identity = first->objectId();
        assert(identity.isValid() && identity == first->objectId() && *ObjectRuntime::instance().resolve(identity) == first);
        std::thread worker(
            [identity]
            {
                auto resolved = ObjectRuntime::instance().resolve(identity);
                assert(!resolved && resolved.error() == EObjectTreeError::WRONG_THREAD);
            }
        );
        worker.join();
        first->~Node();
        auto* second = ::new (memory) Node;
        assert(!ObjectRuntime::instance().resolve(identity) && ObjectRuntime::instance().resolve(identity).error() == EObjectTreeError::CLOSED);
        assert(identity != second->objectId() && *ObjectRuntime::instance().resolve(second->objectId()) == second);
        second->~Node();
        // An identity is a value and never retains the node or a separate control block.
        std::thread release([identity = std::move(identity)]() mutable { identity = {}; });
        release.join();
    }

    void mixedTree()
    {
        Node survivor;
        std::vector<int> order;
        int releases{}, moves{};
        auto first = std::unique_ptr<Node, Deleter>(new Node, Deleter{releases, moves});
        auto second = std::make_unique<Node>();
        first->cleanup = [&] { order.push_back(1); };
        second->cleanup = [&] { order.push_back(2); };
        {
            Node parent, member;
            assert(parent.addChild(member) && parent.addChild(survivor));
            assert(parent.addChild(*first) && parent.addChild(*second));
            parent.clearChildren();
            assert(!releases && order.empty() && !member.parent() && !survivor.parent());
            assert(!first->parent() && !second->parent() && !parent.firstChild());
            assert(parent.addChild(*first) && parent.addChild(*second) && parent.addChild(survivor));
        }
        assert(!survivor.parent() && !first->parent() && !second->parent() && order.empty());
        second.reset();
        first.reset();
        assert(releases == 1 && order == std::vector<int>({2, 1}));
    }

    void refusalAndCleanup()
    {
        Node parent, other, child, sibling;
        assert(other.addChild(child) && other.addChild(sibling));
        parent.beginTreeVisit();
        auto rejected = parent.addChild(child);
        assert(!rejected && rejected.error() == EObjectTreeError::BUSY);
        assert(child.parent() == &other && other.firstChild() == &child && child.nextSibling() == &sibling);
        parent.endTreeVisit();
        assert(parent.addChild(child)); // Reparent is one atomic, non-owning relation change.
        assert(other.firstChild() == &sibling && parent.firstChild() == &child);
        assert(parent.addChild(child) && parent.firstChild() == &child && !child.nextSibling());
        assert(!other.removeChild(child) && child.parent() == &parent);
        auto cycle = child.addChild(parent);
        assert(!cycle && cycle.error() == EObjectTreeError::INVALID_TREE && !parent.parent());
        Node sender;
        unsigned callbacks{};
        auto connection = LuxObject::connect(&sender, &Node::changed, [&]() noexcept {
            auto result = other.addChild(child);
            assert(!result && result.error() == EObjectTreeError::BUSY && child.parent() == &parent);
            ++callbacks;
        });
        assert(connection && sender.emit(sender.changed).direct == 1 && callbacks == 1);
        assert(parent.removeChild(child) && !child.parent() && !parent.firstChild());
    }

    void retirement()
    {
        auto& queue = ObjectRuntime::instance();
        Node parent, sender;
        int destroyed{};
        auto candidate = std::make_unique<Node>();
        candidate->cleanup = [&] { ++destroyed; };
        auto shared = shareOnRuntime(std::move(candidate));
        assert(shared && !candidate && parent.addChild(**shared));
        auto* pointer = shared->get();
        auto connection = LuxObject::connect(&sender, &Node::changed, pointer, [&]() noexcept {
            shared->reset();
            assert(queue.collectRetired() == 0 && !destroyed);
        });
        assert(connection);
        (void)sender.emit(sender.changed);
        assert(queue.pendingRetirements() == 1 && !destroyed);
        assert(queue.collectRetired() == 1 && destroyed == 1 && !parent.firstChild());
        auto late = shareOnRuntime(std::make_unique<Node>());
        assert(late && parent.addChild(**late));
        late->reset();
        parent.clearChildren(); // Unbinding never consumes the pending shared-allocation reclamation.
        assert(queue.collectRetired() == 1 && !queue.pendingRetirements());
        auto worker_candidate = std::make_unique<Node>();
        const auto affinity = std::this_thread::get_id();
        worker_candidate->cleanup = [&] { assert(std::this_thread::get_id() == affinity); ++destroyed; };
        auto worker_owner = shareOnRuntime(std::move(worker_candidate));
        assert(worker_owner && !worker_candidate);
        std::weak_ptr<Node> weak = *worker_owner;
        std::thread worker([owner = std::move(*worker_owner)]() mutable { owner.reset(); });
        worker.join();
        assert(weak.expired() && destroyed == 1 && queue.pendingRetirements() == 1);
        assert(queue.collectRetired() == 1 && destroyed == 2);
    }

    void fixedBatchAndIdentity()
    {
        auto& queue = ObjectRuntime::instance();
        int destroyed{};
        auto second = std::make_unique<Node>();
        second->cleanup = [&] { ++destroyed; };
        auto second_owner = shareOnRuntime(std::move(second));
        assert(second_owner);
        auto first = std::make_unique<Node>();
        first->cleanup = [&] {
            second_owner->reset();
            assert(queue.collectRetired() == 0);
        };
        auto first_owner = shareOnRuntime(std::move(first));
        assert(first_owner);
        first_owner->reset();
        assert(queue.collectRetired() == 1 && !destroyed && queue.pendingRetirements() == 1);
        assert(queue.collectRetired() == 1 && destroyed == 1);
        alignas(Node) std::byte storage[sizeof(Node)];
        auto destroy = [](Node* node) noexcept { std::destroy_at(node); };
        using PlacementOwner = std::unique_ptr<Node, decltype(destroy)>;
        PlacementOwner old{std::construct_at(reinterpret_cast<Node*>(storage)), destroy};
        const auto old_id = old->objectId();
        auto old_owner = shareOnRuntime(std::move(old));
        assert(old_owner);
        old_owner->reset();
        assert(queue.collectRetired() == 1 && !queue.resolve(old_id));
        PlacementOwner replacement{std::construct_at(reinterpret_cast<Node*>(storage)), destroy};
        assert(replacement->objectId() != old_id && !queue.resolve(old_id));
        replacement->cleanup = [&] { ++destroyed; };
        assert(queue.collectRetired() == 0 && destroyed == 1);
        replacement.reset();
        assert(destroyed == 2);
        auto plain = std::make_unique<int>(42);
        auto shared = shareOnRuntime(std::move(plain));
        assert(shared && **shared == 42 && !plain);
        shared->reset();
        assert(queue.collectRetired() == 1);
    }

    void partialConstructionAndDerivedResource()
    {
        struct Composite final : Node
        {
            bool resource_alive{true};
            std::unique_ptr<Node> child;
            ~Composite() override
            {
                child.reset();
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
        assert(owner->addChild(*first));
        owner->child = std::move(first);
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
        auto& queue = ObjectRuntime::instance();
        Node owner{}, sender{};
        int destroyed{};
        auto child = std::make_unique<Node>();
        child->cleanup = [&] { ++destroyed; };
        auto adopted = shareOnRuntime(std::move(child));
        assert(adopted && owner.addChild(**adopted));
        auto connection = LuxObject::connect(
            &sender,
            &Node::changed,
            adopted->get(),
            [&]() noexcept
            {
                adopted->reset();
                assert(queue.collectRetired() == 0 && !destroyed);
            },
            EDelivery::QUEUED
        );
        assert(connection);
        const auto capacity = queue.statistics().capacity_per_batch;
        for (std::size_t i = 0; i < capacity - 1; ++i)
            assert(detail::post(detail::makeMessage([]() noexcept {})) == detail::EPostStatus::POSTED);
        assert(sender.emit(sender.changed).queued == 1);
        assert(sender.emit(sender.changed).full == 1);
        assert(queue.dispatchPending() == capacity && !destroyed);
        assert(queue.collectRetired() == 1 && destroyed == 1);

        for (int round = 0; round != 8; ++round)
        {
            Node tree;
            int count{};
            std::vector<std::unique_ptr<Node>> children;
            auto* parent = &tree;
            for (int depth = 0; depth != 1024; ++depth)
            {
                auto next = std::make_unique<Node>();
                next->cleanup = [&] { ++count; };
                assert(parent->addChild(*next));
                parent = next.get();
                children.push_back(std::move(next));
            }
            tree.clearChildren();
            assert(count == 0);
            children.clear();
            assert(count == 1024 && !tree.firstChild());
            for (int width = 0; width != 2048; ++width)
            {
                auto next = std::make_unique<Node>();
                next->cleanup = [&] { ++count; };
                assert(tree.addChild(*next));
                children.push_back(std::move(next));
            }
            tree.clearChildren();
            assert(count == 1024);
            children.clear();
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
                    assert(!(*target)->parent());
                    auto result = parent->addChild(**target);
                    assert(!result && result.error() == EObjectTreeError::BUSY);
                    ++*rejected;
                }
            }

            void operator()(Node* value) noexcept
            {
                delete value;
            }
        };
        auto& queue = ObjectRuntime::instance();
        Node parent{};
        Node* target{};
        int rejected{};
        auto candidate = std::unique_ptr<Node, ReentrantDeleter>(
            new Node(),
            ReentrantDeleter{parent, target, rejected}
        );
        target = candidate.get();
        auto result = shareOnRuntime(std::move(candidate));
        assert(result && rejected > 0 && !candidate && !target->parent());
        assert(parent.addChild(*target)); // Guard is released only after the owning control block exists.
        result->reset();
        assert(queue.collectRetired() == 1 && !parent.firstChild());

    }

    void wrongThreadRefusal()
    {
        Node parent;
        int releases{}, moves{};
        auto candidate = std::unique_ptr<Node, Deleter>(new Node, Deleter{releases, moves});
        const auto initial_moves = moves;
        auto* address = candidate.get();
        std::thread worker([&] {
            auto refused = parent.addChild(*candidate);
            assert(!refused && refused.error() == EObjectTreeError::WRONG_THREAD);
            auto shared = shareOnRuntime(std::move(candidate));
            assert(!shared && shared.error() == EObjectTreeError::WRONG_THREAD);
        });
        worker.join();
        assert(candidate.get() == address && moves == initial_moves && !releases && !address->parent());

    }

    void finalSafePoint()
    {
        auto& runtime = ObjectRuntime::instance();
        int destroyed{}, delivered{};
        std::shared_ptr<Node> second;
        {
            Node sender, receiver;
            auto connection = LuxObject::connect(
                &sender, &Node::changed, &receiver, [&]() noexcept { ++delivered; }, EDelivery::QUEUED
            );
            assert(connection && sender.emit(sender.changed).queued == 1);
            auto child = std::make_unique<Node>();
            child->cleanup = [&] { ++destroyed; };
            auto shared = shareOnRuntime(std::move(child));
            assert(shared);
            second = std::move(*shared);
            auto first = std::make_unique<Node>();
            first->cleanup = [&] { second.reset(); ++destroyed; };
            auto owner = shareOnRuntime(std::move(first));
            assert(owner);
            owner->reset();
            assert(!destroyed && runtime.pendingRetirements() == 2);
        }
        // Ending a local framework scope cannot close the process runtime. The host still drains
        // fixed retirement batches, without dispatching the now-invalid business callback.
        assert(runtime.collectRetired() == 1 && destroyed == 1 && !second);
        assert(runtime.collectRetired() == 1 && destroyed == 2 && delivered == 0);
        assert(runtime.dispatchPending() == 1 && delivered == 0);
        auto candidate = std::make_unique<Node>();
        candidate->cleanup = [&] { ++destroyed; };
        auto owner = shareOnRuntime(std::move(candidate));
        assert(owner);
        owner->reset();
        assert(runtime.collectRetired() == 1 && destroyed == 3 && runtime.isCurrent());
    }

    int rejectFinalSafePoint(std::string_view mode)
    {
        auto& runtime = ObjectRuntime::instance();
        std::puts("reached final safe-point contract");
        std::fflush(stdout);
        if (mode == "--reject-held")
        {
            auto shared = shareOnRuntime(std::make_unique<Node>());
            assert(shared);
            // Intentionally keep a live owner beyond process Runtime shutdown in this death test.
            (void)new std::shared_ptr<Node>(std::move(*shared));
        }
        else if (mode == "--reject-dispatch")
        {
            Node sender;
            auto receiver = std::make_unique<Node>();
            auto connection = LuxObject::connect(&sender, &Node::changed, receiver.get(), [&]() noexcept {
                receiver.reset();
            });
            assert(connection);
            (void)sender.emit(sender.changed);
        }
        else if (mode == "--reject-foreign")
        {
            std::thread worker([&] { (void)runtime.collectRetired(); });
            worker.join();
        }
        return 0; // --reject-held must fail at the process's actual final safe point.
    }

    void pluginReplacement(const char* path)
    {
        using Library = lux::engine::platform::DynamicLibrary;
        using Make = void (*)(CodeLease, int*, std::shared_ptr<LuxObject>&) noexcept;
        int first_trace[4]{}, second_trace[4]{};
        auto load = [&](int* trace) {
            return std::shared_ptr<Library>(new Library(path), [trace](Library* value) noexcept {
                assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1);
                delete value;
                ++trace[3];
            });
        };
        auto first_library = load(first_trace), second_library = load(second_trace);
        assert(first_library->is_loaded() && second_library->is_loaded());
        auto create = reinterpret_cast<Make>(first_library->get_symbol("make_object"));
        assert(create);
        std::shared_ptr<LuxObject> first, second;
        create(CodeLease::plugin(first_library), first_trace, first);
        create(CodeLease::plugin(second_library), second_trace, second);
        Node parent;
        assert(parent.addChild(*first));
        first_library.reset();
        second_library.reset();
        first = std::move(second);
        assert(!second && !first_trace[0] && !first_trace[3]);
        assert(ObjectRuntime::instance().collectRetired() == 1 && first_trace[3] == 1 && !parent.firstChild());
        assert(!second_trace[0]);
        first.reset();
        assert(ObjectRuntime::instance().collectRetired() == 1 && second_trace[3] == 1);
    }

    void pinnedValue(const char* path)
    {
        using Library = lux::engine::platform::DynamicLibrary;
        using Make = void (*)(CodeLease, int*, std::shared_ptr<const void>&) noexcept;
        int trace[4]{};
        auto library = std::shared_ptr<Library>(new Library(path), [&](Library* value) noexcept {
            assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1);
            delete value;
            ++trace[3];
        });
        assert(library->is_loaded());
        auto create = reinterpret_cast<Make>(library->get_symbol("make_pinned_value"));
        assert(create);
        std::shared_ptr<const void> shared;
        create(CodeLease::plugin(library), trace, shared);
        assert(shared);
        std::weak_ptr<const void> weak = shared;
        auto copy = shared;
        assert(!copy.owner_before(shared) && !shared.owner_before(copy));
        library.reset();
        shared.reset();
        assert(!trace[0] && !trace[1] && !trace[2] && !trace[3] && !weak.expired());
        std::thread worker([owner = std::move(copy)]() mutable { owner.reset(); });
        worker.join();
        assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1 && trace[3] == 1 && weak.expired());
        weak.reset(); // Only the stable host control block remains after actual DLL unload.
    }

    void plugin(const char* path)
    {
        using Library = lux::engine::platform::DynamicLibrary;
        using Make = void (*)(CodeLease, int*, std::shared_ptr<LuxObject>&) noexcept;
        int trace[4]{};
        auto library = std::shared_ptr<Library>(new Library(path), [&](Library* value) noexcept {
            assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1);
            delete value;
            ++trace[3];
        });
        assert(library->is_loaded());
        auto create = reinterpret_cast<Make>(library->get_symbol("make_object"));
        assert(create);
        std::shared_ptr<LuxObject> shared;
        create(CodeLease::plugin(library), trace, shared);
        assert(shared);
        const auto identity = shared->objectId();
        auto runtime = reinterpret_cast<ObjectRuntime* (*)() noexcept>(library->get_symbol("object_runtime"));
        assert(runtime && runtime() == &ObjectRuntime::instance());
        assert(*ObjectRuntime::instance().resolve(identity) == shared.get());
        std::weak_ptr<LuxObject> weak = shared;
        library.reset();
        std::thread worker([owner = std::move(shared)]() mutable { owner.reset(); });
        worker.join();
        assert(!trace[0] && !trace[3] && weak.expired());
        assert(ObjectRuntime::instance().collectRetired() == 1);
        assert(trace[0] == 1 && trace[1] == 1 && trace[2] == 1 && trace[3] == 1);
        assert(!ObjectRuntime::instance().resolve(identity));
        weak.reset(); // Host control block remains safe after the DLL unloads.
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]).starts_with("--reject-"))
        return rejectFinalSafePoint(argv[1]);
    objectIdentities();
    mixedTree();
    refusalAndCleanup();
    retirement();
    fixedBatchAndIdentity();
    partialConstructionAndDerivedResource();
    queuedRemovalAndShapes();
    sharingDeleterReentry();
    wrongThreadRefusal();
    finalSafePoint();
    if (argc == 2)
    {
        plugin(argv[1]);
        pluginReplacement(argv[1]);
        pinnedValue(argv[1]);
    }
    std::puts("Object ownership: mixed tree, refusal, callback, retirement, worker release and DLL tail passed");
}
