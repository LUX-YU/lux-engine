#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <cassert>

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
		ReflectionRegistry::instance().registerClass(std::move(stdstring_view_ref_info));
    }

    void meta_module_init()
    {
		ReflectionRegistry::initRegistry();
		registerSpecialSupportType();
    }

    void meta_module_deinit()
    {
        ReflectionRegistry::destroyRegistry();
    }

	static inline ReflectionRegistry* g_reflection_registry = nullptr;
    void ReflectionRegistry::initRegistry()
    {
        g_reflection_registry = new ReflectionRegistry();

        // Drain all modules that registered via MetaModuleRegistrar static instances.
        qual_type_index_fix_list fix_list;
        for (auto* n = MetaModuleRegistrar::pendingHead(); n; n = n->next)
            n->fn(*g_reflection_registry, fix_list);
        MetaModuleRegistrar::pendingHead() = nullptr; // release references after draining
        meta_register_qual_type_index_fix(*g_reflection_registry, fix_list);
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

    ReflectionRegistry::ReflectionRegistry() = default;

    size_t ReflectionRegistry::registerClass(std::unique_ptr<RefClass> meta)
    {
        return add(class_pool_, class_map_, meta);
    }
    size_t ReflectionRegistry::registerEnum(std::unique_ptr<RefEnum> meta)
    {
        return add(enum_pool_, enum_map_, meta);
    }

    const RefClass* ReflectionRegistry::findClass(std::string_view full) noexcept {
        return find(class_map_, class_pool_, full);
    }

    const RefEnum* ReflectionRegistry::findEnum(std::string_view full) noexcept {
        return find(enum_map_, enum_pool_, full);
    }

    const RefFunction* ReflectionRegistry::findFunction(std::string_view full, std::span<const uint64_t> ids) noexcept
    {
        RefFunctionKey key{ full, {ids.begin(), ids.end()} };
        auto it = func_map_.find(key);
        return (it == func_map_.end()) ? nullptr : func_pool_.at(it->second).get();
    }

    size_t ReflectionRegistry::registerFunction(const RefFunctionKey& k, std::unique_ptr<RefFunction> meta)
    {
        if (auto it = func_map_.find(k); it != func_map_.end()) {
            return it->second;
        }
        size_t id = func_pool_.insert(std::move(meta));
        func_pool_.at(id)->container_index = id;
        func_map_.emplace(k, id);
        return id;
    }

    std::size_t ReflectionRegistry::registerInvokable(const RefInvokable& invokable)
    {
        // Check if already registered
        auto it = invokable_map_.find(invokable.full_name);
        if (it != invokable_map_.end()) {
            return it->second;
        }

        // Add to registry
        std::size_t index = invokable_registry_.size();
        invokable_registry_.push_back(invokable);
        invokable_registry_.back().container_index = index;
        invokable_map_[invokable.full_name] = index;
        return index;
    }

    const RefInvokable* ReflectionRegistry::findInvokableByIndex(std::size_t index) const noexcept
    {
        if (index < invokable_registry_.size()) {
            return &invokable_registry_[index];
        }
        return nullptr;
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
