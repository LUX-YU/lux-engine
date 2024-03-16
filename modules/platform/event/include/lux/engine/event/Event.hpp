#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <functional>
#include <memory>
#include <lux/cxx/compile_time/tuple_traits.hpp>

namespace lux::event
{
	class Event
	{
		template<typename T, uint64_t ID> friend class TEvent;
	public:
		uint64_t id;

	private:
		Event(uint64_t _id) : id(_id) {}
	};

	template<typename T, uint64_t ID>
	class TEvent : public Event
	{
	public:
		template<typename U> constexpr
		TEvent(U&& _data) : Event(ID), data(std::forward<U>(_data)) {}
		TEvent() : Event(ID) {}

		template<typename U>
		void setData(U&& _data) { data = std::forward<U>(_data); }

		const T& getData() const { return data; }
		T& getData() { return data; }

	private:
		T data;
	};

	template<uint64_t ID>
	class TEvent<void, ID> : public Event
	{
	public:
		TEvent() : Event(ID) {}
	};

	template<typename Derived>
	class EventHandler
	{
	public:
		template<typename TEvent>
		std::unique_ptr<TEvent> handleEvent(std::unique_ptr<TEvent> event)
			requires std::is_base_of_v<Event, TEvent>
		{
			return static_cast<Derived*>(this)->handleEvent(std::move(event));
		}
	};

	template<typename T, typename TEvent>
	concept hasHandler = requires(T t)
	{
		{t.handleEvent(std::unique_ptr<TEvent>())} -> std::convertible_to<std::unique_ptr<TEvent>>;
	};

	template<typename... TEvents>
	class EventDispatcher
	{
		template<typename EventType>
		using THandlerFuncPtr = std::function<std::unique_ptr<EventType>(std::unique_ptr<EventType>)>;
		template<typename EventType>
		using THandlerFuncList = std::vector<THandlerFuncPtr<EventType>>;
		using THandlerFuncListTuple = std::tuple<THandlerFuncList<TEvents>...>;

	public:
		using TEventTuple			= std::tuple<TEvents...>;

		template<typename Derived>
		void registerHandler(EventHandler<Derived>* handler)
		{
			lux::cxx::tuple_traits::for_each_type<TEventTuple>(
				[this, handler]<typename TEvent, size_t I>()
				{
					if constexpr(hasHandler<Derived, TEvent>)
					{
						auto wrapper =
						[handler](std::unique_ptr<TEvent> event) -> std::unique_ptr<TEvent>
						{
							return handler->handleEvent(std::move(event));
						};

						auto& func_list = std::get<THandlerFuncList<TEvent>>(functions_tuple);
						func_list.push_back(std::move(wrapper));
					}
				}
			);
		}

	protected:
		template<typename TEvent>
		void dispatch(std::unique_ptr<TEvent> event)
			requires std::is_base_of_v<Event, TEvent>
		{
			auto& func_list = std::get<THandlerFuncList<TEvent>>(functions_tuple);
			for (auto& func : func_list)
			{
				event = func(std::move(event));
			}
		}

	private:
		
		THandlerFuncListTuple functions_tuple;
	};
}
