#pragma once
// ============================================================================
//  FeatureOpSend.hpp — client send-routing for typed ops.
//
//  Given a feature's FeatureOpIds and an op descriptor, route a payload to the
//  RIGHT builder push method purely from the op's kind/opcode — no per-proxy
//  hand-picking of push / pushResource / pushWithReply / pushBulk.
//
//  Client-side; needs the session + request types (heavier than FeatureOps.hpp,
//  so kept separate). Include from a feature's *OperationHandlers.cpp where the
//  proxy methods are defined.
// ============================================================================

#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>   // builder() + push family
#include <lux/engine/render/comm/client/RenderRequest.hpp>   // RenderRequest, RenderRequestFactory

#include <span>
#include <utility>

namespace lux::render
{
    /// Stream / Param / opcode-overridden fire-and-forget op: no reply. No-op if
    /// the feature isn't registered (id == kInvalidTypeId) — same graceful
    /// contract as sendWithReply / sendBulk, so an absent feature degrades to
    /// nothing rather than dispatching an unknown TypeId.
    template <class Op, FeatureOpDesc... Ops>
    inline void send(RenderSession& session, const FeatureOpIds<Ops...>& ids,
                     const typename Op::Payload& payload)
    {
        const TypeId id = ids.template id<Op>();
        if (id != kInvalidTypeId)
            session.builder().push(opcode_of_v<Op>, id, payload);
    }

    /// Reply-bearing op (Resource, or any has_reply payload): returns a
    /// RenderRequest<Reply>. If the feature isn't registered the request stays
    /// unresolved (id == kInvalidTypeId) — same contract as the old proxies.
    template <class Op, FeatureOpDesc... Ops>
    [[nodiscard]] inline auto sendWithReply(RenderSession& session, const FeatureOpIds<Ops...>& ids,
                                            const typename Op::Payload& payload)
    {
        using Reply = typename CommandTraits<typename Op::Payload>::Reply;
        auto [req, cb] = RenderRequestFactory<Reply>::make();
        const TypeId id = ids.template id<Op>();
        if (id == kInvalidTypeId)
            return req;
        if constexpr (opcode_of_v<Op> == opcodes::ResourceOp)
            session.builder().pushResource(id, payload, std::move(cb));
        else
            session.builder().pushWithReply(opcode_of_v<Op>, id, payload, std::move(cb));
        return req;
    }

    /// Bulk op: N payloads in one command. No-op when empty or unregistered.
    template <class Op, FeatureOpDesc... Ops>
    inline void sendBulk(RenderSession& session, const FeatureOpIds<Ops...>& ids,
                         std::span<const typename Op::Payload> items)
    {
        const TypeId id = ids.template id<Op>();
        if (id != kInvalidTypeId && !items.empty())
            session.builder().pushBulk(id, items);
    }

    /// Blob op: append the variable-length bytes to the frame, write the
    /// resulting BlobRef into the payload's declared blob field (Op::blob_field,
    /// a pointer-to-member), then route by reply-ness. The caller fills the rest
    /// of the payload; the blob plumbing is derived from the op descriptor.
    template <class Op, FeatureOpDesc... Ops>
    auto sendBlob(RenderSession& session, const FeatureOpIds<Ops...>& ids,
                  typename Op::Payload payload,
                  std::span<const std::byte> blob_bytes, std::size_t align)
    {
        payload.*(Op::blob_field) = session.builder().pushBlob(blob_bytes, align);
        if constexpr (CommandTraits<typename Op::Payload>::has_reply)
            return sendWithReply<Op>(session, ids, payload);
        else
            send<Op>(session, ids, payload);
    }

} // namespace lux::render
