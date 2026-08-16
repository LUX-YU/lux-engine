#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <cassert>
#include <cstdio>
#include <utility>

namespace lux::meta
{
    // -------------------------------------------------------------------------
    // MetaModuleRegistrar implementation
    // -------------------------------------------------------------------------

    auto MetaModuleRegistrar::pendingHead() noexcept -> MetaModuleRegistrar::Node*&
    {
        static MetaModuleRegistrar::Node* head = nullptr;
        return head;
    }

    MetaModuleRegistrar::MetaModuleRegistrar(InitFn fn) noexcept
        : node_{fn, nullptr}
    {
        // Prepend this node into the pending list.
        // Safe because the list is only drained inside initRegistry(),
        // which must be called after all DLLs have been loaded.
        node_.next = pendingHead();
        pendingHead() = &node_;
    }

    // -------------------------------------------------------------------------

    static void registerSpecialSupportType()
    {
		std::unique_ptr<RefClass> stdstring_ref_info = std::make_unique<RefClass>();
        stdstring_ref_info->name                = "string";
        stdstring_ref_info->full_name           = "std::string";
        stdstring_ref_info->is_abstract         = false;
        stdstring_ref_info->hash                = lux::cxx::type_hash<std::string>();
        stdstring_ref_info->type                = lux::meta::ref_type_of_v<std::string>;
		stdstring_ref_info->construct           = [](void* p) { new (p) std::string(); };
		stdstring_ref_info->destruct            = [](void* p) { static_cast<std::string*>(p)->~basic_string(); };
        lux::meta::ref_class_func_gen<RefClass>(*stdstring_ref_info);
        // Self-link RefType.ptr -> the owning RefClass (generated meta code
        // gets this via the qual_type_index fix-up; hand-registered classes
        // must do it themselves). Without it RuntimeObject::cleanup()
        // dereferences a null RefClass to run the destructor.
        stdstring_ref_info->type.ptr            = stdstring_ref_info.get();
		ReflectionRegistry::instance().registerClass(std::move(stdstring_ref_info));

		std::unique_ptr<RefClass> stdstring_view_ref_info = std::make_unique<RefClass>();
		stdstring_view_ref_info->name           = "string_view";
		stdstring_view_ref_info->full_name      = "std::string_view";
		stdstring_view_ref_info->is_abstract    = false;
		stdstring_view_ref_info->hash           = lux::cxx::type_hash<std::string_view>();
		stdstring_view_ref_info->type           = lux::meta::ref_type_of_v<std::string_view>;
		stdstring_view_ref_info->construct      = [](void* p) { new (p) std::string_view(); };
		stdstring_view_ref_info->destruct       = [](void* p) { static_cast<std::string_view*>(p)->~basic_string_view(); };
		lux::meta::ref_class_func_gen<RefClass>(*stdstring_view_ref_info);
		stdstring_view_ref_info->type.ptr       = stdstring_view_ref_info.get();
		ReflectionRegistry::instance().registerClass(std::move(stdstring_view_ref_info));
    }

    void meta_module_init()
    {
		ReflectionRegistry::initRegistry();
		registerSpecialSupportType();
    }

    void meta_module_drain()
    {
        ReflectionRegistry::drainPending();
    }

    ReflectionRegistrationDraft meta_module_drain_draft()
    {
        return ReflectionRegistry::drainPendingDraft();
    }

    void meta_module_deinit()
    {
        ReflectionRegistry::destroyRegistry();
    }

	static inline ReflectionRegistry* g_reflection_registry = nullptr;

    void ReflectionRegistry::drainPending()
    {
        // Registering into a registry that does not exist would be a null
        // dereference inside the generated code, several frames from the
        // actual mistake. Say what is wrong instead.
        if (g_reflection_registry == nullptr)
        {
            std::fprintf(stderr,
                "[meta] drainPending() before meta_module_init() — ignored. "
                "The host must initialise reflection before loading modules.\n");
            return;
        }

        // Drain modules that chained themselves in via MetaModuleRegistrar
        // static instances. The list holds only what has NOT been drained yet:
        // every drain clears it, so this is naturally incremental and a module
        // is never registered twice.
        qual_type_index_fix_list fix_list;
        for (auto* n = MetaModuleRegistrar::pendingHead(); n; n = n->next)
            n->fn(*g_reflection_registry, fix_list);
        MetaModuleRegistrar::pendingHead() = nullptr; // release references after draining
        meta_register_qual_type_index_fix(*g_reflection_registry, fix_list);
    }

    ReflectionRegistrationDraft ReflectionRegistry::drainPendingDraft()
    {
        if (g_reflection_registry == nullptr)
        {
            auto draft = std::unique_ptr<ReflectionRegistry>{
                new ReflectionRegistry()};
            draft->failRegistration(
                EReflectionRegistrationError::REGISTRY_NOT_INITIALIZED);
            return ReflectionRegistrationDraft{nullptr, std::move(draft)};
        }

        auto draft = std::unique_ptr<ReflectionRegistry>{
            new ReflectionRegistry(*g_reflection_registry)};
        qual_type_index_fix_list fix_list;

        // Detach first. A registrar created while generated callbacks execute
        // belongs to a later module drain and must not be consumed recursively.
        auto* pending = MetaModuleRegistrar::pendingHead();
        MetaModuleRegistrar::pendingHead() = nullptr;
        for (auto* node = pending; node; node = node->next)
            node->fn(*draft, fix_list);

        for (auto& [name, type] : fix_list)
        {
            if (type == nullptr)
            {
                draft->failRegistration(
                    EReflectionRegistrationError::INVALID_QUAL_TYPE_FIX,
                    name);
                continue;
            }
            const auto* ref_class = draft->findClass(name);
            if (ref_class == nullptr)
            {
                draft->failRegistration(
                    EReflectionRegistrationError::QUAL_TYPE_NOT_FOUND,
                    name);
                continue;
            }
            type->ptr = ref_class;
        }
        return ReflectionRegistrationDraft{
            g_reflection_registry,
            std::move(draft)};
    }

    void ReflectionRegistry::initRegistry()
    {
        g_reflection_registry = new ReflectionRegistry();
        // Deliberately delegates rather than repeating the drain: the two must
        // never differ, and "the second drain forgot the qual-type fix-up" is
        // the kind of divergence that shows up as a subtly wrong plugin type
        // months later.
        drainPending();
    }

    void ReflectionRegistry::destroyRegistry()
    {
		delete g_reflection_registry;
        g_reflection_registry = nullptr;
    }

    ReflectionRegistry& ReflectionRegistry::instance() noexcept
    {
		return *g_reflection_registry;
    }

    bool ReflectionRegistry::initialized() noexcept
    {
        return g_reflection_registry != nullptr;
    }

    ReflectionRegistry::ReflectionRegistry() = default;

    ReflectionRegistry::ReflectionRegistry(
        ReflectionRegistry& fallback) noexcept
        : fallback_(&fallback)
        , class_index_base_(fallback.class_pool_.next_id())
        , enum_index_base_(fallback.enum_pool_.next_id())
        , function_index_base_(fallback.func_pool_.next_id())
        , invokable_index_base_(fallback.invokable_registry_.size())
    {}

    void ReflectionRegistry::failRegistration(
        EReflectionRegistrationError error,
        std::string_view name,
        std::string_view conflicting_name)
    {
        if (registration_failure_)
            return;
        registration_failure_.emplace(ReflectionRegistrationFailure{
            error,
            std::string{name},
            std::string{conflicting_name}});
    }

    const RefClass* ReflectionRegistry::findClassByHash(
        std::uint64_t hash) const noexcept
    {
        for (const auto& value : class_pool_.values())
            if (value && value->hash == hash)
                return value.get();
        return fallback_ ? fallback_->findClassByHash(hash) : nullptr;
    }

    size_t ReflectionRegistry::registerClass(std::unique_ptr<RefClass> meta)
    {
        if (!fallback_)
            return add(class_pool_, class_map_, meta);
        if (!meta)
        {
            failRegistration(
                EReflectionRegistrationError::PUBLISH_INVARIANT_BROKEN);
            return class_index_base_ + class_pool_.next_id();
        }
        if (const auto* duplicate = findClass(meta->full_name))
        {
            failRegistration(
                EReflectionRegistrationError::DUPLICATE_CLASS,
                meta->full_name,
                duplicate->full_name);
            return duplicate->container_index;
        }
        if (const auto* collision = findClassByHash(meta->hash))
        {
            failRegistration(
                EReflectionRegistrationError::CLASS_HASH_COLLISION,
                meta->full_name,
                collision->full_name);
            return collision->container_index;
        }
        const auto local = class_pool_.insert(std::move(meta));
        const auto index = class_index_base_ + local;
        class_pool_.at(local)->container_index = index;
        class_map_.emplace(class_pool_.at(local)->full_name, local);
        return index;
    }
    size_t ReflectionRegistry::registerEnum(std::unique_ptr<RefEnum> meta)
    {
        if (!fallback_)
            return add(enum_pool_, enum_map_, meta);
        if (!meta)
        {
            failRegistration(
                EReflectionRegistrationError::PUBLISH_INVARIANT_BROKEN);
            return enum_index_base_ + enum_pool_.next_id();
        }
        if (const auto* duplicate = findEnum(meta->full_name))
        {
            failRegistration(
                EReflectionRegistrationError::DUPLICATE_ENUM,
                meta->full_name,
                duplicate->full_name);
            return duplicate->container_index;
        }
        const auto local = enum_pool_.insert(std::move(meta));
        const auto index = enum_index_base_ + local;
        enum_pool_.at(local)->container_index = index;
        enum_map_.emplace(enum_pool_.at(local)->full_name, local);
        return index;
    }

    const RefClass* ReflectionRegistry::findClass(std::string_view full) noexcept {
        if (const auto* value = find(class_map_, class_pool_, full))
            return value;
        return fallback_ ? fallback_->findClass(full) : nullptr;
    }

    const RefEnum* ReflectionRegistry::findEnum(std::string_view full) noexcept {
        if (const auto* value = find(enum_map_, enum_pool_, full))
            return value;
        return fallback_ ? fallback_->findEnum(full) : nullptr;
    }

    const RefFunction* ReflectionRegistry::findFunction(std::string_view full, std::span<const uint64_t> ids) noexcept
    {
        RefFunctionKey key{ full, {ids.begin(), ids.end()} };
        auto it = func_map_.find(key);
        if (it != func_map_.end())
            return func_pool_.at(it->second).get();
        return fallback_ ? fallback_->findFunction(full, ids) : nullptr;
    }

    size_t ReflectionRegistry::registerFunction(const RefFunctionKey& k, std::unique_ptr<RefFunction> meta)
    {
        if (auto it = func_map_.find(k); it != func_map_.end()) {
            if (fallback_)
                failRegistration(
                    EReflectionRegistrationError::DUPLICATE_FUNCTION,
                    k.name);
            return fallback_ ? function_index_base_ + it->second
                             : it->second;
        }
        if (fallback_)
        {
            if (const auto found = fallback_->func_map_.find(k);
                found != fallback_->func_map_.end())
            {
                failRegistration(
                    EReflectionRegistrationError::DUPLICATE_FUNCTION,
                    k.name);
                return found->second;
            }
        }
        const auto local = func_pool_.insert(std::move(meta));
        const auto index = fallback_ ? function_index_base_ + local : local;
        func_pool_.at(local)->container_index = index;
        func_map_.emplace(k, local);
        return index;
    }

    std::size_t ReflectionRegistry::registerInvokable(const RefInvokable& invokable)
    {
        // Check if already registered
        auto it = invokable_map_.find(invokable.full_name);
        if (it != invokable_map_.end()) {
            if (fallback_)
                failRegistration(
                    EReflectionRegistrationError::DUPLICATE_INVOKABLE,
                    invokable.full_name);
            return fallback_ ? invokable_index_base_ + it->second
                             : it->second;
        }
        if (fallback_)
        {
            if (const auto found = fallback_->invokable_map_.find(
                    invokable.full_name);
                found != fallback_->invokable_map_.end())
            {
                failRegistration(
                    EReflectionRegistrationError::DUPLICATE_INVOKABLE,
                    invokable.full_name);
                return found->second;
            }
        }

        // Add to registry
        const auto local = invokable_registry_.size();
        const auto index = fallback_ ? invokable_index_base_ + local : local;
        invokable_registry_.push_back(invokable);
        invokable_registry_.back().container_index = index;
        invokable_map_[invokable.full_name] = local;
        return index;
    }

    const RefInvokable* ReflectionRegistry::findInvokableByIndex(std::size_t index) const noexcept
    {
        if (fallback_)
        {
            if (index < invokable_index_base_)
                return fallback_->findInvokableByIndex(index);
            const auto local = index - invokable_index_base_;
            return local < invokable_registry_.size()
                ? &invokable_registry_[local]
                : nullptr;
        }
        if (index < invokable_registry_.size()) {
            return &invokable_registry_[index];
        }
        return nullptr;
    }

    bool ReflectionRegistry::publishDraft(
        ReflectionRegistry&& draft) noexcept
    {
        if (draft.fallback_ != this || draft.registration_failure_ ||
            class_pool_.next_id() != draft.class_index_base_ ||
            enum_pool_.next_id() != draft.enum_index_base_ ||
            func_pool_.next_id() != draft.function_index_base_ ||
            invokable_registry_.size() != draft.invokable_index_base_)
            return false;

        for (const auto& [name, local] : draft.class_map_)
            if (class_map_.contains(name) ||
                class_pool_.contains(draft.class_index_base_ + local))
                return false;
        for (const auto& [name, local] : draft.enum_map_)
            if (enum_map_.contains(name) ||
                enum_pool_.contains(draft.enum_index_base_ + local))
                return false;
        for (const auto& [key, local] : draft.func_map_)
            if (func_map_.contains(key) ||
                func_pool_.contains(draft.function_index_base_ + local))
                return false;
        for (const auto& [name, local] : draft.invokable_map_)
            if (invokable_map_.contains(name) ||
                draft.invokable_index_base_ + local !=
                    invokable_registry_.size() + local)
                return false;

        class_pool_.reserve(class_pool_.size() + draft.class_pool_.size());
        enum_pool_.reserve(enum_pool_.size() + draft.enum_pool_.size());
        func_pool_.reserve(func_pool_.size() + draft.func_pool_.size());
        class_map_.reserve(class_map_.size() + draft.class_map_.size());
        enum_map_.reserve(enum_map_.size() + draft.enum_map_.size());
        func_map_.reserve(func_map_.size() + draft.func_map_.size());
        invokable_registry_.reserve(
            invokable_registry_.size() + draft.invokable_registry_.size());
        invokable_map_.reserve(
            invokable_map_.size() + draft.invokable_map_.size());

        const auto class_keys = draft.class_pool_.keys();
        for (const auto local : class_keys)
        {
            std::unique_ptr<RefClass> value;
            if (!draft.class_pool_.extract(local, value) || !value)
                return false;
            const auto index = draft.class_index_base_ + local;
            const auto name = value->full_name;
            if (!class_pool_.try_emplace_at(index, std::move(value)))
                return false;
            class_map_.emplace(name, index);
        }

        const auto enum_keys = draft.enum_pool_.keys();
        for (const auto local : enum_keys)
        {
            std::unique_ptr<RefEnum> value;
            if (!draft.enum_pool_.extract(local, value) || !value)
                return false;
            const auto index = draft.enum_index_base_ + local;
            const auto name = value->full_name;
            if (!enum_pool_.try_emplace_at(index, std::move(value)))
                return false;
            enum_map_.emplace(name, index);
        }

        for (const auto& [key, local] : draft.func_map_)
            func_map_.emplace(key, draft.function_index_base_ + local);
        const auto function_keys = draft.func_pool_.keys();
        for (const auto local : function_keys)
        {
            std::unique_ptr<RefFunction> value;
            if (!draft.func_pool_.extract(local, value) || !value ||
                !func_pool_.try_emplace_at(
                    draft.function_index_base_ + local,
                    std::move(value)))
                return false;
        }

        for (auto& value : draft.invokable_registry_)
        {
            const auto index = invokable_registry_.size();
            invokable_registry_.push_back(std::move(value));
            invokable_registry_.back().container_index = index;
            invokable_map_.emplace(
                invokable_registry_.back().full_name,
                index);
        }
        return true;
    }

    ReflectionRegistrationDraft::ReflectionRegistrationDraft(
        ReflectionRegistry* target,
        std::unique_ptr<ReflectionRegistry> draft) noexcept
        : target_(target), draft_(std::move(draft))
    {}

    ReflectionRegistrationDraft::operator bool() const noexcept
    {
        return target_ != nullptr && draft_ &&
            !draft_->registration_failure_;
    }

    const ReflectionRegistrationFailure&
    ReflectionRegistrationDraft::error() const noexcept
    {
        static const ReflectionRegistrationFailure invalid{
            EReflectionRegistrationError::REGISTRY_NOT_INITIALIZED};
        return draft_ && draft_->registration_failure_
            ? *draft_->registration_failure_
            : invalid;
    }

    lux::cxx::expected<void, ReflectionRegistrationFailure>
    ReflectionRegistrationDraft::commit() noexcept
    {
        if (!static_cast<bool>(*this))
            return lux::cxx::unexpected(error());
        if (!target_->publishDraft(std::move(*draft_)))
        {
            return lux::cxx::unexpected(ReflectionRegistrationFailure{
                EReflectionRegistrationError::PUBLISH_INVARIANT_BROKEN});
        }
        draft_.reset();
        target_ = nullptr;
        return {};
    }

    void meta_register_qual_type_index_fix(ReflectionRegistry& registry, qual_type_index_fix_list& list)
    {
        for (auto& [name, type_ptr] : list)
        {
            auto ref_class = registry.findClass(name);
            assert(ref_class && "[While fix index]:WTF? class meta infomation is nullptr?");
            assert(type_ptr  && "[While fix index]:WTF? type ptr infomation is nullptr?");
			type_ptr->ptr = ref_class;
        }
    }

}
