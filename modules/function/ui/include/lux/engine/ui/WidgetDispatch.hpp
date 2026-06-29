#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/meta/Meta.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <string_view>

namespace lux::ui
{
    /**
     * @brief Context passed to each widget draw function.
     *
     *  - @p id    ImGui unique-id string ("##originalfieldname"), null-terminated.
     *  - @p data  Raw pointer to the field's storage (already offset-adjusted).
     *  - @p annot View into the field's annotation string for min/max/tooltip/etc.
     */
    struct WidgetContext
    {
        const char*                id;    //!< "##fieldname" – stable ImGui id
        void*                      data;  //!< pointer to field storage
        lux::meta::AnnotationView  annot; //!< lightweight annotation view
    };

    /**
     * @brief Signature of a widget draw function.
     *
     *        Must be called from within an active ImGui table row (column 1
     *        is already active when the function is invoked by WidgetDispatch).
     *        ImGui::SetNextItemWidth(-FLT_MIN) is applied before the call.
     */
    using WidgetDrawFn = std::function<void(const WidgetContext&)>;

    /// Identity hasher for pre-computed type hashes (FNV1a uint64_t).
    /// The key is already a well-distributed hash value, so passing it
    /// straight through avoids a second transformation by std::hash<uint64_t>.
    struct IdentityHash
    {
        std::size_t operator()(uint64_t h) const noexcept { return static_cast<std::size_t>(h); }
    };

    /**
     * @class WidgetDispatch
     * @brief Extensible registry that maps C++ types to ImGui widget functions.
     *
     *  Uses a two-level dispatch strategy to avoid hash lookups for primitives:
     *   - Primitive types (bool, int, float, double, …): direct array lookup by EBaseType.
     *   - Record types (Eigen::Vector3f, std::string, …): hash map lookup.
     *
     *  Call @ref registerBuiltins() to pre-populate common types.
     *  Additional types can be registered with @ref registerWidget().
     *
     *  @ref draw() renders a full inspector table row (label column + value column).
     */
    class LUX_FUNCTION_UI_PUBLIC WidgetDispatch
    {
    public:
        /// Pre-populate the table with built-in type widgets
        /// (float, double, int, uint32_t, bool,
        ///  Eigen::Vector3f/4f/Quaternionf, std::string).
        void registerBuiltins();

        /// Register a widget function for type T.
        /// Automatically routes to the primitive array (for bool/int/float/…)
        /// or the record hash-map (for struct/class types).
        template<typename T>
        void registerWidget(WidgetDrawFn fn)
        {
            constexpr auto base = lux::meta::deduce_base<T>();
            if constexpr (base != lux::meta::EBaseType::Record  &&
                          base != lux::meta::EBaseType::Unknown &&
                          base != lux::meta::EBaseType::Void)
            {
                prim_table_[lux::meta::p_to_underlying(base)] = std::move(fn);
            }
            else
            {
                record_table_[lux::cxx::type_hash<T>()] = std::move(fn);
            }
        }

        /// Register a widget for a Record type identified by its compile-time hash.
        /// (Use the typed overload above when the type is known at compile time.)
        void registerWidget(uint64_t type_hash, WidgetDrawFn fn);

        /**
         * @brief Render a full inspector table row for @p field.
         *
         *        - Column 0: label (from "display_name" annotation, or field.name).
         *        - Column 1: value widget dispatched by EBaseType (primitives) or
         *                    type hash (records).
         *
         *        Must be called inside an active ImGui 2-column table.
         *
         * @return true  if a widget was found and drawn.
         * @return false if no widget is registered for this type (caller can fallback).
         */
        bool draw(const lux::meta::RefField& field, void* component_base) const;

    private:
        /// Fallback for record types not in `record_table_`: look up the
        /// type's `RefClass` in `ReflectionRegistry` and render its public
        /// fields recursively under a TreeNode in column 0. Returns false
        /// when the type is not in the registry (caller falls back to hex
        /// display).
        bool drawRecord(const lux::meta::RefField& field,
                        void* component_base) const;

        /// Direct-indexed by EBaseType value (0-13).  Entries for unregistered
        /// primitives are default-constructed (empty std::function == nullptr).
        std::array<WidgetDrawFn, lux::meta::p_base_type_num> prim_table_{};

        /// Hash map used only for Record (struct/class) types.
        /// Uses IdentityHash since the key is already a FNV1a hash.
        std::unordered_map<uint64_t, WidgetDrawFn, IdentityHash> record_table_;
    };

} // namespace lux::ui
