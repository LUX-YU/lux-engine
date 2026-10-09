#pragma once

#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/CodeLease.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lux::flowforge
{
    // Immutable compiler signature, not a second reflection registry. It owns the names and type
    // descriptions used by pins and lowering, including record compatibility and value operations.
    // Record projections do not expose reflected fields/methods; those remain in the metadata provider.
    class LUX_ENGINE_FLOWFORGE_PUBLIC NativeCallDefinition final
    {
    public:
        using Result = FlowForgeResult<std::shared_ptr<const NativeCallDefinition>>;

        [[nodiscard]] static Result create(
            const meta::RefInvokable&,
            object::CodeLease,
            const meta::RefType* receiver = nullptr
        ) noexcept;

        ~NativeCallDefinition();
        NativeCallDefinition(const NativeCallDefinition&) = delete;
        NativeCallDefinition& operator=(const NativeCallDefinition&) = delete;
        NativeCallDefinition(NativeCallDefinition&&) = delete;
        NativeCallDefinition& operator=(NativeCallDefinition&&) = delete;

        [[nodiscard]] const meta::RefInvokable& signature() const noexcept;
        [[nodiscard]] const meta::RefType* receiver() const noexcept;

    private:
        NativeCallDefinition(const meta::RefInvokable&, const meta::RefType*);

        std::vector<std::string> strings_;
        std::vector<meta::RefClass> records_;
        meta::RefInvokable signature_;
        std::optional<meta::RefType> receiver_;
    };
} // namespace lux::flowforge
