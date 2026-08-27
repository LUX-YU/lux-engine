#pragma once
/**
 * @file FrameExtensionRegistry.hpp
 * @brief Named extension-slot registry for RGFrameContext.
 *
 * Features and kernels register named slots at static-init time; the
 * Renderer fills them each frame with feature-owned data pointers.
 * This replaces hardcoded task-specific fields in RGFrameContext.
 *
 * Thread safety: all registration must complete before the first frame.
 * LUX_REGISTER_FRAME_EXTENSION() runs at static initialisation, which
 * satisfies this requirement.
 */

#include <cstdint>
#include <string>
#include <string_view>

#include <lux/cxx/container/HeterogeneousLookup.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    /// Opaque slot ID assigned by FrameExtensionRegistry.
    /// 0 is reserved as invalid; valid IDs start from 1.
    using FrameExtensionSlotId = uint8_t;
    inline constexpr FrameExtensionSlotId kInvalidExtSlot = 0;

    /// Maximum number of extension slots (must fit in FrameExtensionSlotId range).
    inline constexpr uint32_t kMaxFrameExtensionSlots = 32;

    /**
     * @brief Global registry mapping extension names to slot IDs.
     *
     * Thread safety: all registration must complete before the first frame.
     * Cross-DLL: exported (LUX_FUNCTION_PUBLIC) so plugins share one instance.
     */
    class LUX_FUNCTION_PUBLIC FrameExtensionRegistry
    {
    public:
        static FrameExtensionRegistry& instance() noexcept;

        /// Register a named extension slot. Returns its assigned ID.
        /// If the name was already registered, returns the existing ID.
        FrameExtensionSlotId registerSlot(std::string_view name);

        /// Name → slot ID lookup. Returns kInvalidExtSlot when not registered.
        [[nodiscard]] FrameExtensionSlotId idOf(std::string_view name) const noexcept;

    private:
        /// 异构查找的 map:键存 std::string,但可以直接拿 string_view 查。
        ///
        /// 用普通的 `unordered_map<std::string, T>` 时,`idOf` 里那句
        /// `find(std::string(name))` 会为每次查表构造一个临时 string —— 一次可能
        /// 抛 bad_alloc 的堆分配,而 `idOf` 标着 noexcept,于是 OOM 时不是传播
        /// 错误而是直接 std::terminate。透明 hash 让查表根本不构造 string,
        /// noexcept 因此从"谎言"变成"真的"。
        lux::cxx::heterogeneous_map<FrameExtensionSlotId> name_to_id_;
        FrameExtensionSlotId next_id_{1};
    };

    // =========================================================================
    //  LUX_REGISTER_FRAME_EXTENSION macro
    // =========================================================================

#define LUX_REGISTER_FRAME_EXTENSION_PASTE(ext_name, var_name, uid)                                                    \
    namespace                                                                                                          \
    {                                                                                                                  \
        struct _FrameExtRegistrar_##uid                                                                                \
        {                                                                                                              \
            _FrameExtRegistrar_##uid()                                                                                 \
            {                                                                                                          \
                (var_name) = ::lux::render::FrameExtensionRegistry::instance().registerSlot((ext_name));               \
            }                                                                                                          \
        } _frame_ext_registrar_instance_##uid;                                                                         \
    }

#define LUX_REGISTER_FRAME_EXTENSION_IMPL(ext_name, var_name, uid)                                                     \
    LUX_REGISTER_FRAME_EXTENSION_PASTE(ext_name, var_name, uid)

/// Register a named frame extension slot and store its ID in `var_name`.
/// Usage: static FrameExtensionSlotId s_my_slot;
///        LUX_REGISTER_FRAME_EXTENSION("my_feature", s_my_slot)
#define LUX_REGISTER_FRAME_EXTENSION(ext_name, var_name)                                                               \
    LUX_REGISTER_FRAME_EXTENSION_IMPL(ext_name, var_name, __COUNTER__)

} // namespace lux::render
