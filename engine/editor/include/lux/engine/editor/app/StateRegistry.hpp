#pragma once
/**
 * @file StateRegistry.hpp
 * @brief A refcounted, type-erased table of shared editor states ("state
 *        registrar"). The editor's cross-panel coordination surface.
 *
 * Each consumer `ensure<T>()`s the state types it needs and holds the returned
 * `shared_ptr<T>`. The registry keeps only a `weak_ptr`, so a state lives
 * EXACTLY as long as some consumer holds it — when the last dependent is
 * destroyed, the state is destroyed too (no registry-lifetime leak). A
 * canonical state that must persist (e.g. the editor's Selection) is kept alive
 * by its owner (LuxEditor) holding a strong ref for the session.
 *
 * Design notes:
 *  - Storage is `shared_ptr<void>` — STL type erasure with a correct,
 *    captured deleter and NO RTTI (vs a hand-rolled void*+destroy slot).
 *  - Keyed by `lux::cxx::type_hash<T>()` (compile-time FNV-1a, no `typeid`/RTTI),
 *    retrieved via `static_pointer_cast` (also no RTTI). Works with RTTI off.
 *  - The key is already a good hash, so the map uses an IDENTITY hash — it is
 *    not run through `std::hash` a second time.
 *  - Adding a new shared state = define a type + `ensure<NewState>()` where used;
 *    no central class is modified (OCP), and a consumer only couples to the
 *    states it touches.
 *  - The 64-bit type hash is per-build (not ABI-stable; never persist it) and
 *    technically collidable, so a live entry is verified against `type_name<T>()`
 *    on reuse (debug assert).
 *
 * Not thread-safe — single-threaded (UI) use only.
 */

#include <lux/cxx/compile_time/type_info.hpp>   // type_hash / type_name (no RTTI)

#include <cassert>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lux::editor
{
    class StateRegistry
    {
    public:
        StateRegistry() = default;
        StateRegistry(const StateRegistry&)            = delete;
        StateRegistry& operator=(const StateRegistry&) = delete;

        /// Get the shared state of type T, creating it (from @p args, once) if no
        /// live instance exists. The caller holds a `shared_ptr` → the state
        /// lives as long as some consumer holds it. Cache the result; read it
        /// each frame (never null — `make_shared` throws rather than returns 0).
        template <class T, class... Args>
        [[nodiscard]] std::shared_ptr<T> ensure(Args&&... args)
        {
            return ensureImpl<T>(lux::cxx::type_hash<T>(), std::forward<Args>(args)...);
        }

        /// As ensure(), but a NAME lets distinct same-type states coexist
        /// (key = type_hash mixed with the name). Prefer distinct TYPES when you
        /// can — names are stringly-typed. Use a name only when one generic
        /// state type is genuinely reused for different purposes.
        template <class T, class... Args>
        [[nodiscard]] std::shared_ptr<T> ensureNamed(std::string_view name, Args&&... args)
        {
            return ensureImpl<T>(mixName(lux::cxx::type_hash<T>(), name),
                                 std::forward<Args>(args)...);
        }

        /// Live shared state of type T, or nullptr if none is currently held.
        template <class T>
        [[nodiscard]] std::shared_ptr<T> find() noexcept
        {
            const auto it = slots_.find(lux::cxx::type_hash<T>());
            return it == slots_.end()
                       ? nullptr
                       : std::static_pointer_cast<T>(it->second.ref.lock());
        }

    private:
        struct Entry
        {
            std::weak_ptr<void> ref;
            std::string_view    type_name; ///< hash-collision tie-break (static storage)
        };

        // The key is an already-distributed FNV-1a hash; hash it with identity so
        // unordered_map does NOT run std::hash over it again.
        struct IdentityHash
        {
            std::size_t operator()(std::uint64_t k) const noexcept
            {
                return static_cast<std::size_t>(k);
            }
        };

        template <class T, class... Args>
        std::shared_ptr<T> ensureImpl(std::uint64_t key, Args&&... args)
        {
            Entry& e = slots_[key];
            if (auto sp = e.ref.lock())
            {
                assert(e.type_name == lux::cxx::type_name<T>() &&
                       "StateRegistry: type_hash collision between two distinct state types");
                return std::static_pointer_cast<T>(sp);   // no RTTI
            }
            auto sp     = std::make_shared<T>(std::forward<Args>(args)...);
            e.ref       = sp;
            e.type_name = lux::cxx::type_name<T>();
            return sp;
        }

        // FNV-1a over the name, seeded with the type key.
        static std::uint64_t mixName(std::uint64_t type_key, std::string_view name) noexcept
        {
            std::uint64_t h = type_key;
            for (char c : name)
                h = (h ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
            return h;
        }

        std::unordered_map<std::uint64_t, Entry, IdentityHash> slots_;
    };

} // namespace lux::editor
