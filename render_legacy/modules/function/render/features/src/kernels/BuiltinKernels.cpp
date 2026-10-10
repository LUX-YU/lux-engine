#include <algorithm>
#include <lux/engine/function/render/features/BuiltinKernels.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <vector>

namespace lux::render
{
    namespace kernels
    {
        std::span<const KernelDeclaration> meshKernelDeclarations() noexcept;
        std::span<const KernelDeclaration> shadowKernelDeclarations() noexcept;
        std::span<const KernelDeclaration> utilityKernelDeclarations() noexcept;
    } // namespace kernels

    std::span<const KernelDeclaration> builtinKernelDeclarations() noexcept
    {
        static const auto declarations = []
        {
            std::vector<KernelDeclaration> result;
            result.reserve(12);
            for (auto source :
                 {kernels::meshKernelDeclarations(),
                  kernels::shadowKernelDeclarations(),
                  kernels::utilityKernelDeclarations()})
            {
                result.insert(result.end(), source.begin(), source.end());
            }
            return result;
        }();
        return declarations;
    }
} // namespace lux::render
