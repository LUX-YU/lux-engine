#pragma once
#include <lux/engine/core/visibility.h>
#include <entt/entt.hpp>

namespace lux::meta
{
	class Function;
	class EntityObject;
	class Class;

	using entity_id   	= entt::entity;
	using null_entity_t = entt::null_t;
	inline constexpr null_entity_t null_entity = entt::null;

	class EntityRegistry;
	class LUX_CORE_PUBLIC LuxObject
	{
	public:
		LuxObject() = default;
		virtual ~LuxObject() = default;
	};

	class LUX_CORE_PUBLIC EntityRegistry : public entt::registry
	{
		friend class EntityObject;
	public:
		EntityRegistry();
	};

	class LUX_CORE_PUBLIC EntityObject : public LuxObject
	{
	    friend class EntityRegistry;
	public:
	    explicit EntityObject(EntityRegistry& reg)
	        : reg_(&reg), ent_(reg.create()) 
		{
	    }

	    ~EntityObject() override 
		{
	        if (reg_ && ent_ != entt::null && reg_->valid(ent_)) 
			{
	            reg_->destroy(ent_);
	        }
	    }

	    EntityObject(const EntityObject&) = delete;
	    EntityObject& operator=(const EntityObject&) = delete;

	    EntityObject(EntityObject&& rhs) noexcept
	        : LuxObject(std::move(rhs)),
	          reg_(rhs.reg_),
	          ent_(std::exchange(rhs.ent_, entt::null)) {}

	    EntityObject& operator=(EntityObject&& rhs) noexcept 
		{
	        if (this != &rhs) {
	            if (reg_ && ent_ != entt::null && reg_->valid(ent_)) {
	                reg_->destroy(ent_);
	            }
	            reg_ = rhs.reg_;
	            ent_ = std::exchange(rhs.ent_, entt::null);
	        }
	        return *this;
	    }

		template<typename Type, typename... Args>
		decltype(auto) emplace(Args&&... args)
		{
			return reg_->emplace<Type>(ent_, std::forward<Args>(args)...);
		}

		template<typename Type, typename... Args>
		decltype(auto) emplace_or_replace(Args&&... args)
		{
			return reg_->emplace_or_replace<Type>(ent_, std::forward<Args>(args)...);
		}

		template<typename Type, typename... Func>
		decltype(auto) patch(Func&&... func)
		{
			return reg_->patch<Type>(ent_, std::forward<Func>(func)...);
		}

		template<typename Type, typename... Args>
		decltype(auto) replace(Args&&... args)
		{
			return reg_->replace<Type>(ent_, std::forward<Args>(args)...);
		}

		template<typename Type, typename... Other>
		size_t remove()
		{
			return reg_->remove<Type, Other...>(ent_);
		}

		template<typename Type, typename... Other>
		void erase()
		{
			reg_->erase<Type, Other...>(ent_);
		}

		template<typename Func>
		void erase_if(Func&& func)
		{
			reg_->erase_if(ent_, std::forward<Func>(func));
		}

		template<typename... Type>
		[[nodiscard]] auto get() const
		{
			return reg_->template get<Type...>(ent_);
		}

		template<typename... Type>
		[[nodiscard]] auto get()
		{
			return reg_->template get<Type...>(ent_);
		}

		template<typename Type, typename... Args>
		[[nodiscard]] Type& get_or_emplace(Args&&... args)
		{
			return reg_->template get_or_emplace<Type>(ent_, std::forward<Args>(args)...);
		}

		template<typename... Type>
		[[nodiscard]] auto try_get() const
		{
			return reg_->template try_get<Type...>(ent_);
		}

		template<typename... Type>
		[[nodiscard]] auto try_get()
		{
			return reg_->template try_get<Type...>(ent_);
		}

	    // 需要时再临时构造 handle 使用
	    entt::handle handle() { return entt::handle{*reg_, ent_}; }
	    entt::const_handle handle() const { return entt::const_handle{*reg_, ent_}; }

		inline operator entity_id() const { return ent_; }

	private:
	    EntityRegistry* reg_{nullptr};
	    entity_id ent_{entt::null};
	};
}
