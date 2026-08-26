/****************************************************************************************
 * @file   Meta.hpp
 * @brief  Runtime reflection metadata & registry for the Lux engine.
 *
 *         Depends on @ref QualType (see MetaDef.hpp) – no legacy enumeration remains.
 ****************************************************************************************/
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include <span>

#include "MetaDef.hpp"        // QualType & compile‑time helpers

#include <lux/cxx/reflection/runtime/Marker.hpp>
#include <lux/cxx/algorithm/hash.hpp>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/compile_time/type_info.hpp>

#include <lux/engine/core/visibility.h>

namespace lux::meta
{
    //--------------------------------------------------------------------------------------------------
    // Forward declaration
    //--------------------------------------------------------------------------------------------------
    struct RefClass;

    struct TypeTraitBits
    {
        uint8_t is_standard_layout : 1;
        uint8_t is_trivially_constructible : 1;
        uint8_t is_trivially_copyable : 1;
        uint8_t is_trivially_default_constructible : 1;
        uint8_t is_trivially_destructible : 1;
        uint8_t is_trivially_move_assignable : 1;
        uint8_t is_trivially_move_constructible : 1;
    };

    template<typename T> struct type_trait_bits_of
    {
        static constexpr TypeTraitBits value{
            .is_standard_layout                 = std::is_standard_layout_v<T>,
            .is_trivially_constructible         = std::is_trivially_constructible_v<T>,
            .is_trivially_copyable              = std::is_trivially_copyable_v<T>,
            .is_trivially_default_constructible = std::is_trivially_default_constructible_v<T>,
            .is_trivially_destructible          = std::is_trivially_destructible_v<T>,
            .is_trivially_move_assignable       = std::is_trivially_move_assignable_v<T>,
            .is_trivially_move_constructible    = std::is_trivially_move_constructible_v<T>
        };
    };

    template<typename T> constexpr inline TypeTraitBits type_trait_bits_v = type_trait_bits_of<T>::value;

    //--------------------------------------------------------------------------------------------------
    // 1.  Runtime RefType
    //--------------------------------------------------------------------------------------------------
    /**
     * @struct RefType
     * @brief Runtime description – wraps @ref QualType plus extended data.
     */
    struct RefType
    {
        QualType          qtype;        //!< Compact 8‑bit type tag.
        TypeTraitBits     traits;       //!< Type traits (standard layout, trivial, etc.).
		std::string_view  name;         //!< Human‑readable (qualified) name. for class equals RefClass::full_name.
        uint64_t          hash{0};      //!< Hash of the type name.
        std::uint32_t     size{0};      //!< sizeof(type).
        const void*       ptr{nullptr}; //!< Pointer to the type info (class, enum, etc.).

        bool operator==(const RefType& other) const {
            return hash == other.hash && qtype == other.qtype;
        }
    };

    template<typename T>
    struct ref_type_of
    {
        static constexpr RefType value{
            .qtype  = __builtin_qual_type<T>,
            .traits = type_trait_bits_v<T>,
            .name   = lux::cxx::type_name<T>(),
            .hash   = lux::cxx::type_hash<T>(),
            .size   = sizeof(T)
        };
    };

    template<>
    struct ref_type_of<void>
    {
        static constexpr RefType value{
            .qtype = __builtin_qual_type<void>,
            .name = lux::cxx::type_name<void>(),
            .size = 0
        };
    };
    template<typename T> constexpr inline auto ref_type_of_v = ref_type_of<T>::value;

    template<typename T> constexpr inline const RefType* builtin_ref_type_ptr()
    {
        return &ref_type_of_v<T>;
    }

    //--------------------------------------------------------------------------------------------------
    // 2.  Auxiliary enums / aliases
    //--------------------------------------------------------------------------------------------------
    enum class EVisibility { Public, Protected, Private };

    using MethodInvoker = void (*)(void* obj_or_null, void** args, void* ret);
    using CtorFunc = void (*)(void* mem);
    using DtorFunc = void (*)(void* obj);
    using CopyFunc = void (*)(void* obj, const void* src);
    using MoveFunc = void (*)(void* obj, void* src);

    //--------------------------------------------------------------------------------------------------
    // 3.  Field / parameter / function metadata
    //--------------------------------------------------------------------------------------------------

    /**
     * @class AnnotationView
     * @brief Lightweight, zero-allocation view into a field's annotation string.
     *
     *        The string pointed to by the raw pointer MUST outlive this view.
     *        In practice it is always a static constexpr char[] emitted by the
     *        code generator into a .meta.cpp file – lifetime is the whole program.
     *
     *        Expected string format (set by LUX_MEMBER macro + clang annotate):
     *          "luxref::property::member, key1=value1, key2=value2, ..."
     */
    class AnnotationView
    {
    public:
        constexpr explicit AnnotationView(const char* raw) noexcept : _raw(raw) {}
        constexpr AnnotationView() noexcept : _raw(nullptr) {}

        /// Returns the value for @p key, or std::nullopt if not found.
        /// The returned string_view points directly into the .rodata segment –
        /// zero-copy, zero-allocation.
        [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const noexcept
        {
            if (!_raw) return std::nullopt;
            std::string_view raw(_raw);
            // Skip the marker prefix up to and including the first ", "
            const auto first_comma = raw.find(',');
            if (first_comma == std::string_view::npos) return std::nullopt;
            raw = raw.substr(first_comma + 1);
            while (!raw.empty())
            {
                // skip leading whitespace
                const auto ws = raw.find_first_not_of(" \t");
                if (ws == std::string_view::npos) break;
                raw = raw.substr(ws);
                // Match both `key=value` and the whitespace-preserving form
                // emitted when the source annotation spells `key = value`.
                // The generator deliberately retains source formatting, so
                // consumers must not make the semantic key depend on it.
                if (raw.size() > key.size() &&
                    raw.substr(0, key.size()) == key)
                {
                    auto remainder = raw.substr(key.size());
                    const auto separator =
                        remainder.find_first_not_of(" \t");
                    if (separator != std::string_view::npos &&
                        remainder[separator] == '=')
                    {
                        auto val = remainder.substr(separator + 1);
                        const auto value_start =
                            val.find_first_not_of(" \t");
                        if (value_start == std::string_view::npos)
                            return std::string_view{};
                        val = val.substr(value_start);
                        const auto comma = val.find(',');
                        if (comma != std::string_view::npos)
                            val = val.substr(0, comma);
                        // trim trailing whitespace
                        const auto last = val.find_last_not_of(" \t");
                        if (last != std::string_view::npos)
                            val = val.substr(0, last + 1);
                        return val;
                    }
                }
                // not this key – skip to next comma
                const auto next_comma = raw.find(',');
                if (next_comma == std::string_view::npos) break;
                raw = raw.substr(next_comma + 1);
            }
            return std::nullopt;
        }

        /// Returns true if @p key exists in the annotation.
        [[nodiscard]] bool has(std::string_view key) const noexcept { return get(key).has_value(); }

        /// Returns true if there is no annotation string.
        [[nodiscard]] bool empty() const noexcept { return _raw == nullptr || _raw[0] == '\0'; }

    private:
        const char* _raw{ nullptr };
    };

    struct RefField
    {
        std::string_view name;
        RefType          type;
        EVisibility      visibility{ EVisibility::Public };
        RefClass*        owner_class{ nullptr };
        std::uint32_t    offset{ 0 };
        bool             is_const{ false };
        bool             is_volatile{ false };

        /// Points to a static constexpr char[] emitted by the code generator.
        /// nullptr when the field carries no LUX_MEMBER annotation.
        const char*      annotation_str{ nullptr };

        /// Returns a lightweight view into the annotation string.
        [[nodiscard]] AnnotationView annotations() const noexcept { return AnnotationView(annotation_str); }
    };

    struct RefStaticField
    {
        std::string_view name;
        RefType          type;
        std::string_view template_primary;
        EVisibility      visibility{ EVisibility::Public };
        RefClass*        owner_class{ nullptr };
        const void*      address{ nullptr };
        bool             is_const{ false };
        bool             is_volatile{ false };
        const char*      annotation_str{ nullptr };

        [[nodiscard]] AnnotationView annotations() const noexcept
        {
            return AnnotationView(annotation_str);
        }
    };

    struct RefParam
    {
        // This is the name of the parameter in the function signature, not the name of type.
        std::string_view name; 
        RefType          type;
        std::string_view value_type_name;
        std::uint64_t    value_type_hash{ 0 };
        bool             is_out{ false };
    };

    struct RefInvokable
    {
        std::string_view name;
        std::string_view full_name;
        RefType          return_type;
        std::string_view type_signature;
        std::vector<RefParam> parameters;

        bool             is_variadic{ false };
        MethodInvoker    invoker{ nullptr };
        std::size_t      container_index{ 0 };  // Index in the invokable registry
    };

    struct RefFunction
    {
        RefInvokable  invokable;
        std::size_t   container_index{ 0 };
    };

    struct RefMethod
    {
        RefInvokable  invokable;
        RefClass*     owner_class{ nullptr };

        EVisibility   visibility{ EVisibility::Public };
        bool          is_static{ false };
        bool          is_virtual{ false };
        bool          is_const{ false };
        bool          is_noexcept{ false };
        const char*   annotation_str{ nullptr };

        [[nodiscard]] AnnotationView annotations() const noexcept
        {
            return AnnotationView(annotation_str);
        }
    };

    //--------------------------------------------------------------------------------------------------
    // 4.  Class & enum metadata
    //--------------------------------------------------------------------------------------------------
    struct RefClass
    {
        std::string_view            name;
        std::string_view            full_name;
        std::uint64_t               hash{ 0 };
        RefType                     type;

        CtorFunc                    construct{ nullptr };
        DtorFunc                    destruct{ nullptr };
        CopyFunc                    copy{ nullptr };
        CopyFunc                    copy_construct{nullptr};
        MoveFunc                    move{ nullptr };
        MoveFunc                    move_construct{nullptr};

        std::string_view            construct_symbol;
        std::string_view            destruct_symbol;

        std::vector<std::uint64_t>  parent_chain;

        bool                        is_abstract{ false };

        std::vector<RefField>       fields;
        std::vector<RefStaticField> static_fields;
        std::vector<RefMethod>      methods;

        std::size_t                 container_index{ 0 };

        /// Points to a static constexpr char[] emitted by the code generator
        /// (mirrors `RefField::annotation_str`). nullptr when the class
        /// carries no `LUX_TYPE_INFO(runtime, key=value, ...)` arguments.
        /// Format matches AnnotationView: marker prefix then `key=value`
        /// pairs separated by commas.
        const char*                 annotation_str{ nullptr };

        /// Lightweight zero-allocation view onto `annotation_str`. Returns
        /// `std::nullopt` from `.get(key)` when the annotation is empty or
        /// missing the key — same semantics as `RefField::annotations()`.
        [[nodiscard]] AnnotationView annotations() const noexcept
            { return AnnotationView(annotation_str); }
    };

    // constructor and destructor will assign in the generated codes
    template<typename T> void ref_class_func_gen(T& obj)
    {
        if constexpr (std::is_copy_assignable_v<T>)
        {
            obj.copy =
            [](void* obj, const void* src)
            {
                auto obj_raw = static_cast<T*>(obj);
                auto src_raw = static_cast<const T*>(src);
                *obj_raw = *src_raw;
            };
        }

        if constexpr (std::is_copy_constructible_v<T>)
        {
            obj.copy_construct =
            [](void* mem, const void* other)
            {
                auto other_raw = static_cast<const T*>(other);
                new (mem) T(*other_raw);
            };
        }

        if constexpr (std::is_move_assignable_v<T>)
        {
            obj.move =
            [](void* obj, void* src)
            {
                auto obj_raw = static_cast<T*>(obj);
                auto src_raw = static_cast<T*>(src);
                *obj_raw = std::move(*src_raw);
            };
        }

        if constexpr (std::is_move_constructible_v<T>)
        {
            obj.move_construct = 
            [](void* mem, void* other)
            {
                auto other_raw = static_cast<T*>(other);
                new (mem) T(std::move(*other_raw));
            };
        }
    }

    struct RefEnumValue { std::string name; std::int64_t value; };

    struct RefEnum
    {
        std::string_view          name;
        std::string_view          full_name;
        bool                      is_scoped{ false };
        std::vector<RefEnumValue> values;
        std::size_t               container_index{ 0 };
    };

    //--------------------------------------------------------------------------------------------------
    // 5.  Function key for map lookup
    //--------------------------------------------------------------------------------------------------
    struct RefFunctionKey
    {
        std::string_view            name;
        std::vector<std::uint64_t>  param_types;  //!< lux::cxx::type_hash for each param

        bool operator==(const RefFunctionKey&) const = default;
    };

    struct RefFuncKeyHash
    {
        std::size_t operator()(const RefFunctionKey& k) const noexcept
        {
            std::size_t h = lux::cxx::algorithm::fnv1a(k.name);
            for (auto t : k.param_types)
                h = (h ^ t) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    //--------------------------------------------------------------------------------------------------
    // 6.  ReflectionRegistry – singleton
    //--------------------------------------------------------------------------------------------------

    template<typename T>
    using MetaPool = lux::cxx::AutoSparseSet<std::unique_ptr<T>>;

    enum class EReflectionRegistrationError : std::uint8_t
    {
        NONE,
        REGISTRY_NOT_INITIALIZED,
        DUPLICATE_CLASS,
        CLASS_HASH_COLLISION,
        DUPLICATE_ENUM,
        DUPLICATE_FUNCTION,
        DUPLICATE_INVOKABLE,
        QUAL_TYPE_NOT_FOUND,
        INVALID_QUAL_TYPE_FIX,
        PUBLISH_INVARIANT_BROKEN
    };

    struct ReflectionRegistrationFailure final
    {
        EReflectionRegistrationError error{
            EReflectionRegistrationError::NONE};
        std::string name;
        std::string conflicting_name;
    };

    class ReflectionRegistrationDraft;

    /**
     * @class ReflectionRegistry
     * @brief Stores and serves all runtime reflection metadata.
     */
    class LUX_CORE_PUBLIC ReflectionRegistry
    {
    public:
        static void initRegistry();

        /// Register any module that chained itself in SINCE the last drain,
        /// WITHOUT recreating the registry.
        ///
        /// This is what a plugin host calls after dlopen/LoadLibrary. A module
        /// loaded at runtime runs its MetaModuleRegistrar constructors on load,
        /// which only chain into the pending list — and initRegistry() already
        /// emptied that list and would, if called again, build a NEW registry
        /// and silently discard every type registered so far.
        ///
        /// Skipping this step fails quietly, which is the reason it exists as
        /// its own entry point: an unregistered component type does not throw,
        /// it makes Scene::load take its "skip if type unknown" branch for
        /// every component the plugin owns — the file loads, the entity count
        /// is right, and every entity owns nothing.
        static void drainPending();

        /// Drain the current pending module chain into an unpublished registry
        /// overlay. Lookups in the overlay can see the live registry, while
        /// live readers cannot see the overlay until the returned draft is
        /// committed. This is the dynamic-extension registration boundary.
        [[nodiscard]] static ReflectionRegistrationDraft drainPendingDraft();

        static void destroyRegistry();
        static ReflectionRegistry& instance() noexcept;
        /// True after initRegistry()/meta_module_init(). instance() on an
        /// uninitialized registry is a null dereference — callers that may
        /// run in a host that forgot meta_module_init() should check first.
        static bool initialized() noexcept;

        ReflectionRegistry(const ReflectionRegistry&) = delete;
        ReflectionRegistry& operator=(const ReflectionRegistry&) = delete;
        ReflectionRegistry(ReflectionRegistry&&) = delete;
        ReflectionRegistry& operator=(ReflectionRegistry&&) = delete;

        // -- registration --------------------------------------------------------
        std::size_t registerClass(std::unique_ptr<RefClass> meta);
        std::size_t registerEnum(std::unique_ptr<RefEnum> meta);
        std::size_t registerFunction(const RefFunctionKey& key, std::unique_ptr<RefFunction> meta);
        std::size_t registerInvokable(const RefInvokable& invokable);

        // -- lookup --------------------------------------------------------------
        [[nodiscard]] const RefClass* findClass(std::string_view full) noexcept;
        [[nodiscard]] const RefClass* findClassByHash(
            std::uint64_t hash
        ) const noexcept;
        [[nodiscard]] const RefEnum* findEnum(std::string_view full) noexcept;
        [[nodiscard]] const RefFunction* findFunction(std::string_view full, std::span<const std::uint64_t> ids = {}) noexcept;
        
        [[nodiscard]] const RefInvokable* findInvokableByIndex(std::size_t index) const noexcept;

        template<typename... Ts>
        const RefFunction* findFunctionTyped(std::string_view full) noexcept
        {
            static constexpr std::uint64_t ids[] = { lux::cxx::type_hash<Ts>()... };
            return findFunction(full, ids);
        }

        // -- iteration -----------------------------------------------------------
        const auto& classes()   const noexcept { return class_pool_.values(); }
        const auto& enums()     const noexcept { return enum_pool_.values(); }
        const auto& functions() const noexcept { return func_pool_.values(); }
        const auto& invokables() const noexcept { return invokable_registry_; }

    private:
        friend class ReflectionRegistrationDraft;
        ReflectionRegistry();
        explicit ReflectionRegistry(ReflectionRegistry& fallback) noexcept;

        void failRegistration(
            EReflectionRegistrationError error,
            std::string_view name = {},
            std::string_view conflicting_name = {});
        [[nodiscard]] bool publishDraft(ReflectionRegistry&& draft) noexcept;
        // pool storage
        MetaPool<RefClass>    class_pool_;
        MetaPool<RefEnum>     enum_pool_;
        MetaPool<RefFunction> func_pool_;
        std::vector<RefInvokable> invokable_registry_; // Registry for all invokable entities

        // maps → pool indices
        using MetaMap = std::unordered_map<std::string_view, std::size_t>;
        using FuncMap = std::unordered_map<RefFunctionKey, std::size_t, RefFuncKeyHash>;

        MetaMap class_map_;
        MetaMap enum_map_;
        FuncMap func_map_;
        MetaMap invokable_map_; // Maps full_name to index

        ReflectionRegistry* fallback_{nullptr};
        std::size_t class_index_base_{0u};
        std::size_t enum_index_base_{0u};
        std::size_t function_index_base_{0u};
        std::size_t invokable_index_base_{0u};
        std::optional<ReflectionRegistrationFailure> registration_failure_;

        // helper for pool insertion
        template<typename Map, typename Pool, typename Ptr>
        static std::size_t add(Pool& pool, Map& map, Ptr& meta_ptr)
        {
            const std::string_view key = meta_ptr->full_name;
            if (auto it = map.find(key); it != map.end())
                return it->second;

            std::size_t id = pool.insert(std::move(meta_ptr));
            pool.at(id)->container_index = id;
            map.emplace(key, id);
            return id;
        }

        template<typename Map, typename Pool>
        static auto find(Map& map, Pool& pool, std::string_view key) noexcept
            -> decltype(pool.at(0).get())
        {
            auto it = map.find(key);
            return (it == map.end()) ? nullptr : pool.at(it->second).get();
        }
    };

    /// Move-only unpublished reflection registration. Destruction without
    /// commit discards every generated metadata owner and leaves the live
    /// registry unchanged.
    class LUX_CORE_PUBLIC ReflectionRegistrationDraft final
    {
    public:
        ReflectionRegistrationDraft() noexcept = default;
        ~ReflectionRegistrationDraft() = default;
        ReflectionRegistrationDraft(const ReflectionRegistrationDraft&) =
            delete;
        ReflectionRegistrationDraft& operator=(
            const ReflectionRegistrationDraft&) = delete;
        ReflectionRegistrationDraft(
            ReflectionRegistrationDraft&&) noexcept = default;
        ReflectionRegistrationDraft& operator=(
            ReflectionRegistrationDraft&&) noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const ReflectionRegistrationFailure& error() const
            noexcept;
        [[nodiscard]] lux::cxx::expected<
            void,
            ReflectionRegistrationFailure>
        commit() noexcept;

    private:
        friend class ReflectionRegistry;
        ReflectionRegistrationDraft(
            ReflectionRegistry* target,
            std::unique_ptr<ReflectionRegistry> draft) noexcept;

        ReflectionRegistry* target_{nullptr};
        std::unique_ptr<ReflectionRegistry> draft_;
    };

    //--------------------------------------------------------------------------------------------------
    // 7.  Module‑lifetime helpers
    //--------------------------------------------------------------------------------------------------
    LUX_CORE_PUBLIC void meta_module_init();
    /// Call after loading a module at RUNTIME (plugin dlopen/LoadLibrary) so
    /// the types it brought become visible. See ReflectionRegistry::drainPending
    /// — calling meta_module_init() a second time instead would discard every
    /// type registered so far.
    LUX_CORE_PUBLIC void meta_module_drain();
    [[nodiscard]] LUX_CORE_PUBLIC ReflectionRegistrationDraft
    meta_module_drain_draft();
    LUX_CORE_PUBLIC void meta_module_deinit();

	// Use for generating meta data
    using qual_type_index_fix_list = std::vector<std::pair<std::string_view, lux::meta::RefType*>>;
	LUX_CORE_PUBLIC void meta_register_qual_type_index_fix(ReflectionRegistry&, qual_type_index_fix_list&);

    //--------------------------------------------------------------------------------------------------
    // 8.  MetaModuleRegistrar – zero-overhead static self-registration
    //
    //  Generated .meta.cpp files each place one file-scope static instance:
    //
    //      static lux::meta::MetaModuleRegistrar _registrar(&my_module_meta);
    //
    //  The constructor chains the init function into an intrusive linked list.
    //  ReflectionRegistry::initRegistry() drains that list so every linked
    //  module registers its types automatically – callers never need to name
    //  individual registration functions.
    //--------------------------------------------------------------------------------------------------
    class LUX_CORE_PUBLIC MetaModuleRegistrar
    {
    public:
        using InitFn = void(*)(ReflectionRegistry&, qual_type_index_fix_list&);

        /// Chains @p fn into the pending-init list.  Safe to call before
        /// ReflectionRegistry::initRegistry() (e.g. during DLL load).
        explicit MetaModuleRegistrar(InitFn fn) noexcept;

    private:
        friend class ReflectionRegistry;

        struct Node { InitFn fn; Node* next; };

        /// Returns a reference to the (magic-static) linked-list head.
        /// Thread-safe by the C++11 magic-static guarantee.
        static Node*& pendingHead() noexcept;

        Node node_;   ///< Storage for this registrar's list node.
    };

} // namespace lux::meta
