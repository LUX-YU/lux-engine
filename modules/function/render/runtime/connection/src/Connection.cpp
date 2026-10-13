#include <lux/engine/render/connection/Connection.hpp>
#include <limits>
#include <exception>
#include <lux/cxx/concurrent/LockFreeQueue.hpp>

namespace lux::render::connection
{
    static_assert(std::atomic<void*>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    struct ServerIdentity final
    {
        std::shared_ptr<const OperationDirectory> operations;
    };

    struct RequestMessage final
    {
        RequestHeader header;
        Packet packet;
    };

    struct ConnectionBacking final
    {
        ConnectionBacking(std::shared_ptr<const ServerIdentity> identity, ConnectionConfig options) noexcept
            : server(std::move(identity)), config(options), requests(options.request_capacity),
              replies(options.reply_capacity), reply_storage(options.reply_capacity + 1, options.message_reserve) {}

        std::shared_ptr<const ServerIdentity> server;
        ConnectionConfig config;
        cxx::SpscLockFreeRingQueue<RequestMessage> requests;
        cxx::SpscLockFreeRingQueue<ReplyMessage> replies;
        MessageStorage reply_storage;
        std::atomic<bool> client_open{true};
        std::atomic<bool> server_open{true};
        std::atomic<bool> accepted{false};
        std::thread::id server_owner;
        std::size_t active_work = 0; // Server owner only, including deferred work.
    };

    struct PendingConnect final
    {
        std::shared_ptr<ConnectionBacking> backing;
        PendingConnect* next = nullptr;
    };

    namespace
    {
        std::atomic<LocalListener*> published_listener{nullptr};

        void requireThread(std::thread::id owner) noexcept
        {
            if (owner != std::this_thread::get_id()) std::terminate();
        }

        bool validCapacity(std::size_t capacity) noexcept
        {
            return capacity > 0 && capacity <= (std::numeric_limits<std::uint32_t>::max)() / 2 &&
                (capacity & (capacity - 1)) == 0;
        }
    }

    MessageStorage::MessageStorage(std::size_t count, std::size_t reserve) noexcept
    {
        blocks_.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto block = std::make_shared<std::vector<std::byte>>();
            block->reserve(reserve);
            blocks_.push_back(std::move(block));
        }
    }

    LocalListener::LocalListener(
        std::string_view address, std::shared_ptr<const OperationDirectory> operations, ConnectionConfig config
    ) noexcept
        : address_(address), server_(std::make_shared<ServerIdentity>(ServerIdentity{std::move(operations)})),
          config_(config), owner_(std::this_thread::get_id()) {}

    RenderResult<std::unique_ptr<LocalListener>> LocalListener::listen(
        std::string_view address, std::shared_ptr<const OperationDirectory> operations, ConnectionConfig config
    ) noexcept
    {
        const bool is_invalid_config = !validCapacity(config.request_capacity) ||
            !validCapacity(config.reply_capacity) || config.byte_budget == 0;
        if (address.empty() || !operations || is_invalid_config) return cxx::unexpected(RenderError{kProtocolContract});
        if (published_listener.load(std::memory_order_acquire)) return cxx::unexpected(RenderError{kAddress});
        for (const auto& entry : *operations)
        {
            if (operations->find(entry.id) != &entry) return cxx::unexpected(RenderError{kProtocolContract});
            if (auto valid = validateOperation(entry.descriptor); !valid) return cxx::unexpected(valid.error());
            for (const auto& other : *operations)
            {
                if (&entry != &other && entry.descriptor.type == other.descriptor.type)
                    return cxx::unexpected(RenderError{kProtocolContract});
            }
        }
        auto listener = std::unique_ptr<LocalListener>(new LocalListener(address, std::move(operations), config));
        published_listener.store(listener.get(), std::memory_order_release);
        return listener;
    }

    LocalListener::~LocalListener() { stop(); }

    std::size_t LocalListener::accept(std::size_t budget) noexcept
    {
        requireThread(owner_);
        if (stopped_) return 0;
        if (!accepting_) accepting_ = pending_.exchange(nullptr, std::memory_order_acquire);
        std::size_t count = 0;
        while (accepting_ && count < budget)
        {
            std::unique_ptr<PendingConnect> node(accepting_);
            accepting_ = node->next;
            if (node->backing->client_open.load(std::memory_order_acquire))
            {
                endpoints_.push_back(std::make_unique<ServerConnectionEndpoint>(std::move(node->backing)));
            }
            ++count;
        }
        return count;
    }

    std::span<const std::unique_ptr<ServerConnectionEndpoint>> LocalListener::endpoints() const noexcept
    {
        requireThread(owner_);
        return endpoints_;
    }

    std::size_t LocalListener::collectDisconnected() noexcept
    {
        requireThread(owner_);
        const auto before = endpoints_.size();
        std::erase_if(endpoints_, [](const auto& endpoint) { return endpoint->reclaimable(); });
        return before - endpoints_.size();
    }

    void LocalListener::stop() noexcept
    {
        requireThread(owner_);
        if (stopped_) return;
        stopped_ = true;
        published_listener.store(nullptr, std::memory_order_release);
        auto release = [](PendingConnect* node) noexcept
        {
            while (node)
            {
                std::unique_ptr<PendingConnect> owned(node);
                node = owned->next;
                owned->backing->server_open.store(false, std::memory_order_release);
            }
        };
        release(pending_.exchange(nullptr, std::memory_order_acquire));
        release(std::exchange(accepting_, nullptr));
        for (auto& endpoint : endpoints_) endpoint->close();
        endpoints_.clear();
    }

    RenderResult<std::unique_ptr<ClientConnectionEndpoint>>
    ClientConnectionEndpoint::connect(std::string_view address) noexcept
    {
        // Host guarantees Listener lifetime over every concurrent connect call.
        auto* listener = published_listener.load(std::memory_order_acquire);
        if (!listener || listener->address_ != address) return cxx::unexpected(RenderError{kAddress});
        auto backing = std::make_shared<ConnectionBacking>(listener->server_, listener->config_);
        auto endpoint = std::unique_ptr<ClientConnectionEndpoint>(new ClientConnectionEndpoint(backing));
        auto node = std::make_unique<PendingConnect>();
        node->backing = std::move(backing);
        auto* head = listener->pending_.load(std::memory_order_relaxed);
        do
        {
            node->next = head;
            // No dereference of head: accept may reclaim an old head during retry.
        } while (!listener->pending_.compare_exchange_weak(
            head, node.get(), std::memory_order_release, std::memory_order_relaxed
        ));
        (void)node.release();
        return endpoint;
    }

    ClientConnectionEndpoint::ClientConnectionEndpoint(std::shared_ptr<ConnectionBacking> backing) noexcept
        : backing_(std::move(backing)), owner_(std::this_thread::get_id()),
          pending_(backing_->config.reply_capacity),
          storage_(backing_->config.reply_capacity + 1, backing_->config.message_reserve) {}

    ClientConnectionEndpoint::~ClientConnectionEndpoint() { close(); }

    void ClientConnectionEndpoint::adoptCurrentThread() noexcept { owner_ = std::this_thread::get_id(); }

    bool ClientConnectionEndpoint::ready() const noexcept
    {
        return backing_->accepted.load(std::memory_order_acquire) &&
            backing_->server_open.load(std::memory_order_acquire) &&
            backing_->client_open.load(std::memory_order_acquire);
    }

    const ConnectionConfig& ClientConnectionEndpoint::config() const noexcept { return backing_->config; }

    RenderResult<BoundOperation> ClientConnectionEndpoint::bind(const OperationDescriptor& descriptor) const noexcept
    {
        requireThread(owner_);
        if (!ready()) return cxx::unexpected(RenderError{kClosed});
        if (auto valid = validateOperation(descriptor); !valid) return cxx::unexpected(valid.error());
        const auto& operations = *backing_->server->operations;
        for (std::size_t i = 0; i < operations.size(); ++i)
        {
            const auto& candidate = operations.values()[i].descriptor;
            if (candidate.type != descriptor.type) continue;
            if (auto match = compatibleOperations(candidate, descriptor); !match)
            {
                return cxx::unexpected(match.error());
            }
            BoundOperation result;
            result.server_ = backing_->server;
            result.id_ = operations.values()[i].id;
            return result;
        }
        return cxx::unexpected(RenderError{kStale});
    }

    RenderResult<OperationDescriptor>
    ClientConnectionEndpoint::validateSubmit(const BoundOperation& operation) const noexcept
    {
        requireThread(owner_);
        if (!ready()) return cxx::unexpected(RenderError{kClosed});
        if (operation.server_ != backing_->server) return cxx::unexpected(RenderError{kOwner});
        const auto* descriptor = backing_->server->operations->find(operation.id_);
        if (!descriptor) return cxx::unexpected(RenderError{kStale});
        if (pending_.size() >= config().reply_capacity) return cxx::unexpected(RenderError{kCapacity});
        return descriptor->descriptor;
    }

    RenderResult<RequestId> ClientConnectionEndpoint::submit(const BoundOperation& operation, Packet& packet) noexcept
    {
        auto descriptor = validateSubmit(operation);
        if (!descriptor) return cxx::unexpected(descriptor.error());
        if (packet.attachmentView().size() > config().max_attachments) return cxx::unexpected(RenderError{kCapacity});
        std::size_t bytes = packet.bytes.size();
        for (const auto& attachment : packet.attachmentView())
        {
            if (attachment.size() > config().byte_budget - (std::min)(bytes, config().byte_budget))
            {
                return cxx::unexpected(RenderError{kCapacity});
            }
            bytes += attachment.size();
        }
        if (bytes > config().byte_budget - in_flight_bytes_) return cxx::unexpected(RenderError{kCapacity});
        if (auto valid = validateWire(
            {descriptor->payload, packet.bytes, packet.attachmentView()}, {config().byte_budget, config().byte_budget}
        ); !valid) return cxx::unexpected(valid.error());
        auto id = pending_.emplace(Pending{{}, bytes, false});
        pending_.find(id)->id = id;
        // Construct the queue element only after its capacity check: failed publication retains packet.
        const auto pushed = backing_->requests.tryEmplace(RequestHeader{id, operation.id_}, std::move(packet));
        if (pushed != cxx::EQueuePushResult::ACCEPTED)
        {
            pending_.erase(id);
            return cxx::unexpected(RenderError{kCapacity});
        }
        packet = {};
        in_flight_bytes_ += bytes;
        return id;
    }

    RenderResult<std::optional<ReplyMessage>> ClientConnectionEndpoint::poll() noexcept
    {
        requireThread(owner_);
        // Bounded by reserved credits, even if every returned reply was abandoned.
        const auto budget = config().reply_capacity;
        for (std::size_t i = 0; i < budget; ++i)
        {
            auto popped = backing_->replies.tryPopValue();
            std::optional<ReplyMessage> reply = std::move(popped.value);
            if (!reply)
            {
                const bool is_closed = !backing_->server_open.load(std::memory_order_acquire) ||
                    !backing_->client_open.load(std::memory_order_acquire);
                if (!is_closed || pending_.empty()) return std::optional<ReplyMessage>{};
                // A terminal published before server_open=false wins over unknown.
                popped = backing_->replies.tryPopValue();
                reply = std::move(popped.value);
                if (!reply) reply = ReplyMessage{{pending_.values().back().id, EOperationOutcome::OUTCOME_UNKNOWN,
                    {kClosed}}, {}, {}};
            }
            auto* pending = pending_.find(reply->header.request);
            if (!pending)
            {
                if (!backing_->client_open.load(std::memory_order_acquire)) continue;
                return cxx::unexpected(RenderError{kStale});
            }
            const auto abandoned = pending->abandoned;
            in_flight_bytes_ -= pending->bytes;
            pending_.erase(reply->header.request);
            if (!abandoned) return reply;
        }
        return std::optional<ReplyMessage>{};
    }

    RenderResult<void> ClientConnectionEndpoint::abandon(RequestId request) noexcept
    {
        requireThread(owner_);
        auto* pending = pending_.find(request);
        if (!pending || pending->abandoned) return cxx::unexpected(RenderError{kStale});
        pending->abandoned = true;
        return {};
    }

    void ClientConnectionEndpoint::close() noexcept
    {
        requireThread(owner_);
        backing_->client_open.store(false, std::memory_order_release);
    }

    std::size_t ClientConnectionEndpoint::requestDepth() const noexcept { return backing_->requests.size(); }

    std::size_t ClientConnectionEndpoint::replyDepth() const noexcept { return backing_->replies.size(); }

    ServerConnectionEndpoint::ServerConnectionEndpoint(std::shared_ptr<ConnectionBacking> backing) noexcept
        : backing_(std::move(backing))
    {
        backing_->server_owner = std::this_thread::get_id();
        backing_->accepted.store(true, std::memory_order_release);
    }

    ServerConnectionEndpoint::~ServerConnectionEndpoint() { close(); }

    std::optional<ServerWork> ServerConnectionEndpoint::receive() noexcept
    {
        requireThread(backing_->server_owner);
        if (!backing_->server_open.load(std::memory_order_acquire)) return {};
        auto popped = backing_->requests.tryPopValue();
        if (!popped.value) return {};
        return ServerWork(backing_, popped.value->header, std::move(popped.value->packet));
    }

    void ServerConnectionEndpoint::close() noexcept
    {
        requireThread(backing_->server_owner);
        backing_->server_open.store(false, std::memory_order_release);
    }

    bool ServerConnectionEndpoint::disconnected() const noexcept
    {
        return !backing_->client_open.load(std::memory_order_acquire);
    }

    bool ServerConnectionEndpoint::reclaimable() const noexcept
    {
        requireThread(backing_->server_owner);
        return disconnected() && backing_->active_work == 0 && backing_->requests.empty();
    }

    ServerWork::ServerWork(std::shared_ptr<ConnectionBacking> backing, RequestHeader header, Packet input) noexcept
        : backing_(std::move(backing)), header_(header), input_(std::move(input))
    {
        ++backing_->active_work;
    }

    ServerWork::~ServerWork()
    {
        if (backing_) (void)cancel();
    }

    ServerWork::ServerWork(ServerWork&& other) noexcept
        : backing_(std::move(other.backing_)), header_(other.header_), input_(std::move(other.input_)) {}

    ServerWork& ServerWork::operator=(ServerWork&& other) noexcept
    {
        if (this == &other) return *this;
        if (backing_) (void)cancel();
        backing_ = std::move(other.backing_);
        header_ = other.header_;
        input_ = std::move(other.input_);
        return *this;
    }

    const OperationDescriptor* ServerWork::descriptor() const noexcept
    {
        if (!backing_) return nullptr;
        requireThread(backing_->server_owner);
        auto* entry = backing_->server->operations->find(header_.operation);
        return entry ? &entry->descriptor : nullptr;
    }

    WireLimits ServerWork::limits() const noexcept
    {
        return {backing_->config.byte_budget, backing_->config.byte_budget};
    }

    MessageStorage& ServerWork::storage() noexcept { return backing_->reply_storage; }

    RenderResult<void> ServerWork::succeed() noexcept
    {
        const auto* operation = descriptor();
        if (!operation) return cxx::unexpected(RenderError{kStale});
        if (operation->reply) return fail({kProtocolContract});
        return finish(EOperationOutcome::SUCCESS, {}, {});
    }

    RenderResult<void> ServerWork::fail(RenderError error) noexcept
    {
        return finish(EOperationOutcome::FAILED, error, {});
    }

    RenderResult<void> ServerWork::cancel() noexcept { return finish(EOperationOutcome::CANCELLED, {kCancelled}, {}); }

    RenderResult<void> ServerWork::finish(EOperationOutcome outcome, RenderError error, Packet result) noexcept
    {
        if (!backing_) return cxx::unexpected(RenderError{kStale});
        requireThread(backing_->server_owner);
        if (outcome == EOperationOutcome::SUCCESS)
        {
            const auto* operation = descriptor();
            if (operation && operation->reply)
            {
                auto valid = validateWire({*operation->reply, result.bytes, result.attachmentView()}, limits());
                if (!valid)
                {
                    outcome = EOperationOutcome::FAILED;
                    error = valid.error();
                    result = {};
                }
            }
        }
        auto backing = std::move(backing_);
        --backing->active_work;
        if (!backing->server_open.load(std::memory_order_acquire))
        {
            input_ = {};
            return cxx::unexpected(RenderError{kClosed});
        }
        // Only this server owner writes replies. Outstanding <= capacity even if Client pauses.
        const auto pushed = backing->replies.tryEmplace(
            ReplyHeader{header_.request, outcome, error}, std::move(result), std::move(input_)
        );
        if (pushed != cxx::EQueuePushResult::ACCEPTED) std::terminate();
        input_ = {};
        return {};
    }

    std::span<const error::ErrorDescriptor> connectionErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.connection.capacity", "Connection would block", error::ERecovery::RETRYABLE},
            {"lux.render.connection.closed", "Connection lost; outcome may be unknown", error::ERecovery::PERMANENT},
            {"lux.render.connection.stale", "Stale or completed identity", error::ERecovery::PERMANENT},
            {"lux.render.connection.owner", "Foreign server binding", error::ERecovery::PERMANENT},
            {"lux.render.connection.address", "Address absent or already listening", error::ERecovery::PERMANENT},
            {"lux.render.connection.cancelled", "Uncompleted server work cancelled", error::ERecovery::PERMANENT}
        };
        return descriptors;
    }
}
