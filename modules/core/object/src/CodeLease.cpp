#include <lux/engine/object/CodeLease.hpp>

namespace lux::object::detail
{
    namespace
    {
        struct CodeOwner final
        {
            CodeLease code;
            std::shared_ptr<const void> value;

            void operator()(const void*) noexcept
            {
                value.reset();
                code = CodeLease::builtin();
            }
        };
    } // namespace

    std::shared_ptr<const void> pinCodeOwner(CodeLease code, std::shared_ptr<const void> value) noexcept
    {
        auto* pointer = value.get();
        return std::shared_ptr<const void>(pointer, CodeOwner{std::move(code), std::move(value)});
    }
} // namespace lux::object::detail
