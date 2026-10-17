#pragma once

#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace runtime_test
{
    using namespace lux::render;
    namespace ipc = lux::platform::ipc;

    // Test application state. Request numbering and the discovered catalog are outside the library.
    struct Peer final
    {
        ipc::IpcRequester requester;
        CommandId list_commands;
        CommandList commands;
        std::uint64_t next = 1;
    };

    inline RenderResult<std::unique_ptr<Peer>> connect(std::string_view name, CommandId list_commands) noexcept
    {
        auto peer = ipc::IpcCore::instance().createRequester(name);
        if (!peer) return lux::cxx::unexpected(RenderError{kRuntimeState});
        return std::make_unique<Peer>(Peer{std::move(*peer), list_commands});
    }

    template <RenderCommand C> RenderResult<CommandId> find(const Peer& peer) noexcept
    {
        const auto found = std::ranges::find(peer.commands.commands, C::name, &CommandInfo::name);
        if (found == peer.commands.commands.end()) return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
        const auto& wanted = commandDescriptor<C>();
        if (found->request != wanted.request || found->response != wanted.response)
            return lux::cxx::unexpected(RenderError{kProtocolContract});
        return found->id;
    }

    template <RenderCommand C> RenderResult<RequestId> send(Peer& peer, CommandId command,
        typename C::Request input) noexcept
    {
        const RequestId id{peer.next++};
        ipc::IpcMessage message;
        message.emplace<typename C::Request>(std::move(input));
        if (!peer.requester.trySend(commandTag(command), id.value, message)) return lux::cxx::unexpected(RenderError{kRuntimeState});
        return id;
    }

    inline ipc::IpcMessage wait(Peer& peer, RequestId id) noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        ipc::IpcMessage message;
        for (;;)
        {
            std::uint64_t correlation = 0;
            const auto status = peer.requester.tryReceive(correlation, message);
            if (status == ipc::EIpcReceive::CLOSED || std::chrono::steady_clock::now() >= deadline) std::abort();
            if (status == ipc::EIpcReceive::RECEIVED)
            {
                if (correlation != id.value) std::abort();
                return message;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    template <RenderCommand C> typename C::Response result(ipc::IpcMessage message) noexcept
    {
        if (message.typeHash() == lux::cxx::type_hash<RenderError>())
            return lux::cxx::unexpected(message.get<RenderError>());
        return std::move(message.get<typename C::Response>());
    }

    template <RenderCommand C> typename C::Response coldCall(Peer& peer,
        typename C::Request input) noexcept
    {
        auto command = find<C>(peer);
        if (!command) return lux::cxx::unexpected(command.error());
        auto sent = send<C>(peer, *command, std::move(input));
        if (!sent) return lux::cxx::unexpected(sent.error());
        auto message = wait(peer, *sent);
        if (message.typeHash() == lux::cxx::type_hash<RenderError>())
            return lux::cxx::unexpected(message.get<RenderError>());
        return std::move(message.get<typename C::Response>());
    }

    inline RenderResult<void> refresh(Peer& peer) noexcept
    {
        auto sent = send<ListCommands>(peer, peer.list_commands, {});
        if (!sent) return lux::cxx::unexpected(sent.error());
        auto message = wait(peer, *sent);
        if (message.typeHash() == lux::cxx::type_hash<RenderError>())
            return lux::cxx::unexpected(message.get<RenderError>());
        auto& response = message.get<ListCommands::Response>();
        if (!response) return lux::cxx::unexpected(response.error());
        peer.commands = std::move(*response);
        return {};
    }

    inline RenderResult<StorageId> prepareStorage(Peer& peer, std::string_view type, std::string key,
        VStorageOwner owner = GlobalStorageOwner{}) noexcept
    {
        auto directory = coldCall<ListStorages>(peer, {});
        if (!directory) return lux::cxx::unexpected(directory.error());
        for (const auto& candidate : directory->storages)
            if (candidate.name == type)
                return coldCall<PrepareStorage>(peer, {{std::move(owner), candidate.id, std::move(key)}, {}});
        return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
    }
}
