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

#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp> // builder() + push family
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp> // RenderRequest, RenderRequestFactory
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>

#include <span>
#include <utility>

namespace lux::render
{
    /// Stream / Param / opcode-overridden fire-and-forget op: no reply. No-op if
    /// the feature isn't registered (id == kInvalidTypeId) — same graceful
    /// contract as sendWithReply / sendBulk, so an absent feature degrades to
    /// nothing rather than dispatching an unknown TypeId.
    template <class Op, FeatureOpDesc... Ops>
    inline void send(RenderProgramSession& session, const FeatureOpIds<Ops...>& ids, const typename Op::Payload& payload)
    {
        static_assert(Op::lane == EOperationLane::Program);
        const TypeId id = ids.template id<Op>();
        if (id != kInvalidTypeId)
            session.builder().push(opcode_of_v<Op>, id, payload);
    }

    /// Reply-bearing op (Resource, or any has_reply payload): returns a
    /// RenderRequest<Reply>. A missing operation settles immediately as a
    /// structured dispatch failure; an unresolved request is never returned.
    template <class Op, FeatureOpDesc... Ops>
    [[nodiscard]] inline auto
    sendWithReply(RenderProgramSession& session, const FeatureOpIds<Ops...>& ids, const typename Op::Payload& payload)
    {
        static_assert(Op::lane == EOperationLane::Program);
        using Reply = typename CommandTraits<typename Op::Payload>::Reply;
        const TypeId id = ids.template id<Op>();
        if (id == kInvalidTypeId)
            return RenderRequestFactory<Reply>::makeImmediateFailure(
                renderError<err::comm::FeatureOperationUnavailable>()
            );
        auto [req, cb] = RenderRequestFactory<Reply>::make();
        if constexpr (opcode_of_v<Op> == opcodes::ResourceOp)
            session.builder().pushResource(id, payload, std::move(cb));
        else
            session.builder().pushWithReply(opcode_of_v<Op>, id, payload, std::move(cb));
        return req;
    }

    /// Bulk op: N payloads in one command. No-op when empty or unregistered.
    template <class Op, FeatureOpDesc... Ops>
    inline void
    sendBulk(RenderProgramSession& session, const FeatureOpIds<Ops...>& ids, std::span<const typename Op::Payload> items)
    {
        static_assert(Op::lane == EOperationLane::Program);
        const TypeId id = ids.template id<Op>();
        if (id != kInvalidTypeId && !items.empty())
            session.builder().pushBulk(id, items);
    }

    /// Blob op: append the variable-length bytes to the frame, write the
    /// resulting BlobRef into the payload's declared blob field (Op::blob_field,
    /// a pointer-to-member), then route by reply-ness. The caller fills the rest
    /// of the payload; the blob plumbing is derived from the op descriptor.
    template <class Op, FeatureOpDesc... Ops>
    auto sendBlob(
        RenderProgramSession& session,
        const FeatureOpIds<Ops...>& ids,
        typename Op::Payload payload,
        std::span<const std::byte> blob_bytes,
        std::size_t align
    )
    {
        static_assert(Op::lane == EOperationLane::Program);
        payload.*(Op::blob_field) = session.builder().pushBlob(blob_bytes, align);
        if constexpr (CommandTraits<typename Op::Payload>::has_reply)
            return sendWithReply<Op>(session, ids, payload);
        else
            send<Op>(session, ids, payload);
    }

    template <class Op, FeatureOpDesc... Ops>
    inline void
    send(RenderControlSession& session, const FeatureOpIds<Ops...>& ids, const typename Op::Payload& payload)
    {
        static_assert(Op::lane == EOperationLane::Control);
        const TypeId id = ids.template id<Op>();
        if (id != kInvalidTypeId)
            session.send(opcode_of_v<Op>, id, payload);
    }

    template <class Op, FeatureOpDesc... Ops>
    [[nodiscard]] inline auto
    sendWithReply(RenderControlSession& session, const FeatureOpIds<Ops...>& ids, const typename Op::Payload& payload)
    {
        static_assert(Op::lane == EOperationLane::Control);
        using Reply = typename CommandTraits<typename Op::Payload>::Reply;
        const TypeId id = ids.template id<Op>();
        if (id == kInvalidTypeId)
            return RenderRequestFactory<Reply>::makeImmediateFailure(
                renderError<err::comm::FeatureOperationUnavailable>()
            );
        return session.template request<Reply>(opcode_of_v<Op>, id, payload);
    }

    template <class Op, FeatureOpDesc... Ops>
    [[nodiscard]] inline UploadSubmitNoReplyResult
    send(const RenderUploadClient& session, const FeatureOpIds<Ops...>& ids, const typename Op::Payload& payload)
    {
        static_assert(Op::lane == EOperationLane::Upload);
        const TypeId id = ids.template id<Op>();
        if (id == kInvalidTypeId)
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        return session.trySubmitNoReply(
            [&](RenderUploadClient::Builder& builder) { builder.push(opcode_of_v<Op>, id, payload); }
        );
    }

    template <class Op, FeatureOpDesc... Ops>
    [[nodiscard]] inline auto sendWithReply(
        const RenderUploadClient& session,
        const FeatureOpIds<Ops...>& ids,
        const typename Op::Payload& payload
    )
    {
        static_assert(Op::lane == EOperationLane::Upload);
        using Reply = typename CommandTraits<typename Op::Payload>::Reply;
        const TypeId id = ids.template id<Op>();
        if (id == kInvalidTypeId)
            return UploadSubmitResult<Reply>{lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID)};
        return session.template trySubmit<Reply>([&](RenderUploadClient::Builder& builder) {
            if constexpr (opcode_of_v<Op> == opcodes::ResourceOp)
                builder.pushPreparedResource(id, payload);
            else
                static_assert(opcode_of_v<Op> == opcodes::ResourceOp, "prepared uploads currently require ResourceOp");
        }
        );
    }

    template <class Op, FeatureOpDesc... Ops>
    [[nodiscard]] inline auto sendBlob(
        const RenderUploadClient& session,
        const FeatureOpIds<Ops...>& ids,
        typename Op::Payload payload,
        std::span<const std::byte> blob_bytes,
        std::size_t align
    )
    {
        static_assert(Op::lane == EOperationLane::Upload);
        const TypeId id = ids.template id<Op>();
        if constexpr (CommandTraits<typename Op::Payload>::has_reply)
        {
            using Reply = typename CommandTraits<typename Op::Payload>::Reply;
            if (id == kInvalidTypeId)
                return UploadSubmitResult<Reply>{lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID)};
            return session.template trySubmit<Reply>([&](RenderUploadClient::Builder& builder) {
                payload.*(Op::blob_field) = builder.pushBlob(blob_bytes, align);
                if constexpr (opcode_of_v<Op> == opcodes::ResourceOp)
                    builder.pushPreparedResource(id, payload);
                else
                    static_assert(
                        opcode_of_v<Op> == opcodes::ResourceOp,
                        "prepared uploads currently require ResourceOp");
            }
            );
        }
        else
        {
            if (id == kInvalidTypeId)
                return UploadSubmitNoReplyResult{lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID)};
            return session.trySubmitNoReply([&](RenderUploadClient::Builder& builder) {
                payload.*(Op::blob_field) = builder.pushBlob(blob_bytes, align);
                builder.push(opcode_of_v<Op>, id, payload);
            }
            );
        }
    }

} // namespace lux::render
