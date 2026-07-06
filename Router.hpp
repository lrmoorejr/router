#pragma once

/*
 * Copyright 2026 L. Richard Moore Jr.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <typeindex>
#include <unordered_map>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <mutex>

// Ensure.hpp is an optional dependency: if it's available (either as part of this
// checkout or vendored alongside this header), use its ensure()/throw_if() for a
// formatted diagnostic on failure; otherwise fall back to equivalent local
// implementations so this header still works standalone.
#if __has_include("commons/Ensure.hpp")
	#include "commons/Ensure.hpp"
#elif __has_include("Ensure.hpp")
	#include "Ensure.hpp"
#else
	#include <cassert>
	// Guard against Ensure.hpp having already been included under a path our
	// __has_include checks above don't know about (e.g. vendored elsewhere as
	// "3rdparty/Ensure.hpp") -- COMMONS_ENSURE_HPP is defined by Ensure.hpp
	// itself, so this still catches that case even under an unknown filename.
	#if !defined(COMMONS_ENSURE_HPP) && !defined(ensure)
		#define ensure(condition, ...) assert((condition))
	#endif
	// (throw_if is a function template, not a macro, so #ifndef can't guard
	// it directly -- redefining it without this check would be a hard error.)
	// A second guard (distinct from COMMONS_ENSURE_HPP) covers the case where two
	// headers using this same standalone fallback are included together.
	#if !defined(COMMONS_ENSURE_HPP) && !defined(COMMONS_THROW_IF_FALLBACK_DEFINED)
	#define COMMONS_THROW_IF_FALLBACK_DEFINED
	template<class T, class... Args>
	constexpr inline void throw_if(bool condition, Args&&... args) {
		if (condition)
			throw T(std::forward<Args>(args)...);
	}
	#endif
#endif

namespace rt {
	/**
	 * @brief Opaque handle identifying one handler registered with Router::addHandler(), for use
	 * with Router::removeHandler().  Carries the event type (via std::type_index) plus a
	 * per-type registration index; callers should treat it as an opaque token.
	 */
	struct HandlerReference : public std::type_index {
		HandlerReference(std::type_index type) : std::type_index(type) {}
		unsigned int index = 0;
	};

	/**
	 * @brief Router is a std::type_index-keyed pub/sub event dispatcher.  Register a handler for
	 * an event type with addHandler(), then broadcast events of that type to every registered
	 * handler with handle().  If an event type T defines a `static void defaultHandler(const T&)`,
	 * it is registered automatically the first time T is seen (via addHandler(), handle(), or
	 * hasHandlers()), ahead of any handler added explicitly.  Router itself is not thread-safe;
	 * see ThreadSafeRouter for a drop-in replacement that adds mutex protection.
	 *
	 */
	class Router {
	public:
		/**
		 * @brief Registers an event handler with this container.  Any events of the specified
		 * template type sent to this container will result in a callback.  Handlers run in
		 * reverse registration order (the most recently added handler fires first); see handle().
		 *
		 * @tparam T Event class
		 * @param handler Callback
		 * @return A handle whose only purpose is to identify the handler for use with removeHandler()
		 */
		template<class T> HandlerReference addHandler(std::function<void(const T&)> handler) {
			HandlerReference reference(typeid(T));
			reference.index = getHandlers<T>().addHandler(std::move(handler));
			return reference;
		}

		/**
		 * @brief Removes the specified handler.
		 *
		 * @param id The id returned from addHandler().  Must identify a type that still has at
		 * least one registered handler; passing an id for a type with none is a usage error
		 * (checked via throw_if()).
		 */
		void removeHandler(const HandlerReference& id) {
			auto iter = handlersMap.find(id);
			throw_if<std::invalid_argument>(iter == handlersMap.end(), "removeHandler called for a type with no registered handlers");
			iter->second->removeHandler(id.index);
		}

		/**
		 * @brief Invokes all registered event handlers for the specified event class type, in
		 * reverse registration order (the most recently added handler fires first).  Handlers
		 * added or removed by a handler during this call do not affect the current dispatch.
		 *
		 * @tparam T Event class
		 * @param event Event details.  This can be allocated on the stack since it is passed by
		 * reference to handlers.
		 */
		template<class T> void handle(const T& event)  {
			getHandlers<T>().send(event);
		}

		/**
		 * @brief Queries whether there are any handlers for the specified event type.
		 *
		 * @tparam T Event class
		 * @return true Yes, there are handlers
		 * @return false No handlers registered.
		 */
		template<class T> bool hasHandlers() {
			return !getHandlers<T>().empty();
		}

	protected:
		//
		// Event support
		//
		struct HandlersBase {
			virtual ~HandlersBase() {}
			virtual bool removeHandler(unsigned int id) = 0;
		};

		template<class T> class Handlers : public HandlersBase {
		public:
			void send(const T& event) {
				// Short circuit if there are no handlers.
				if(handlers.empty()) return;
				// While dispatching, additions/removals are deferred (see addHandler()/
				// removeHandler() below) rather than applied to `handlers` directly: erasing
				// from or reallocating that vector while one of its entries is still
				// executing further up this same call stack would destroy or move a
				// std::function that's mid-invocation.  Deferring means the common case
				// (no mutation during dispatch) never has to copy the handler list just to
				// dispatch safely.
				++dispatchDepth;
				const int count = static_cast<int>(handlers.size());
				for(int handlerIndex = count - 1; handlerIndex >= 0; handlerIndex--)
					handlers[handlerIndex].second(event);
				if(--dispatchDepth == 0)
					applyPending();
			}
			unsigned int addHandler(std::function<void(const T&)> handler) {
				unsigned int id = ++nextId;
				if(dispatchDepth > 0)
					pendingAdds.push_back({id, std::move(handler)});
				else
					handlers.push_back({id, std::move(handler)});
				return id;
			}
			bool removeHandler(unsigned int id) override {
				if(dispatchDepth == 0)
					return eraseNow(id);

				for(const auto& entry : handlers)
					if(entry.first == id) {
						pendingRemoves.push_back(id);
						return true;
					}
				for(const auto& entry : pendingAdds)
					if(entry.first == id) {
						pendingRemoves.push_back(id);
						return true;
					}
				return false;
			}
			bool empty() { return handlers.empty() && pendingAdds.empty(); }

			std::vector<std::pair<unsigned int,std::function<void(const T&)>>> handlers;
			unsigned int nextId = 0;

		private:
			bool eraseNow(unsigned int id) {
				for(auto iter = handlers.begin(); iter != handlers.end(); ++iter) {
					if(iter->first == id) {
						handlers.erase(iter);
						return true;
					}
				}
				return false;
			}

			// Applies deferred mutations once the outermost send() call for this event type has
			// finished.  Adds are applied before removes so that a handler added and then removed
			// within the same dispatch (neither of which took effect while dispatching) nets out
			// to "never actually registered", rather than lingering because the id it removed
			// wasn't in `handlers` yet.
			void applyPending() {
				for(auto& entry : pendingAdds)
					handlers.push_back(std::move(entry));
				pendingAdds.clear();
				for(unsigned int id : pendingRemoves)
					eraseNow(id);
				pendingRemoves.clear();
			}

			int dispatchDepth = 0;
			std::vector<std::pair<unsigned int,std::function<void(const T&)>>> pendingAdds;
			std::vector<unsigned int> pendingRemoves;
		};

		// Default case: defaultHandler not found
		template <typename T, typename = void>
		struct DefaultHandlerHelper {
			static void addDefault(Handlers<T>*) {}
		};

		// Specialization for when 'defaultHandler' exists
		template <typename T>
		struct DefaultHandlerHelper<T, std::void_t<decltype(T::defaultHandler)>> {
			static void addDefault(Handlers<T>* handlers) {
				handlers->addHandler(T::defaultHandler);
			}
		}; 

		template<class T> Handlers<T>& getHandlers() {
			const std::type_index typeIndex = std::type_index(typeid(T));
			auto iter = handlersMap.find(typeIndex);
			if(iter != handlersMap.end()) {
				return *static_cast<Handlers<T>*>(iter->second.get());
			}
			Handlers<T>* handlers = new Handlers<T>();
			DefaultHandlerHelper<T>::addDefault(handlers);
			handlersMap.emplace(typeIndex, handlers);
			return *handlers;
		}

		std::unordered_map<std::type_index,std::unique_ptr<HandlersBase>> handlersMap;
	};

	/**
	 * @brief ThreadSafeRouter is a drop-in replacement for Router.  It adds a mutex in the public
	 * API to ensure thread safety.  The mutex is recursive, so a handler invoked from handle()
	 * may safely call back into addHandler(), removeHandler(), or handle() on the same router
	 * without deadlocking.
	 *
	 */
	class ThreadSafeRouter : protected Router {
	public:
		/**
		 * @brief Registers an event handler with this container.  Any events of the specified
		 * template type sent to this container will result in a callback.  Handlers run in
		 * reverse registration order (the most recently added handler fires first); see handle().
		 *
		 * @tparam T Event class
		 * @param handler Callback
		 * @return A handle whose only purpose is to identify the handler for use with removeHandler()
		 */
		template<class T> HandlerReference addHandler(std::function<void(const T&)> handler) {
			const std::lock_guard<std::recursive_mutex> lock(mutex);
			return Router::addHandler(handler);
		}

		/**
		 * @brief Removes the specified handler.
		 *
		 * @param id The id returned from addHandler().  Must identify a type that still has at
		 * least one registered handler; passing an id for a type with none is a usage error
		 * (checked via throw_if()).
		 */
		 void removeHandler(const HandlerReference& id) {
			const std::lock_guard<std::recursive_mutex> lock(mutex);
			Router::removeHandler(id);
		}

		/**
		 * @brief Invokes all registered event handlers for the specified event class type, in
		 * reverse registration order (the most recently added handler fires first).  Handlers
		 * added or removed by a handler during this call do not affect the current dispatch.
		 *
		 * @tparam T Event class
		 * @param event Event details.  This can be allocated on the stack since it is passed by
		 * reference to handlers.
		 */
		template<class T> void handle(const T& event)  {
			const std::lock_guard<std::recursive_mutex> lock(mutex);
			Router::handle(event);
		}

		/**
		 * @brief Queries whether there are any handlers for the specified event type.
		 *
		 * @tparam T Event class
		 * @return true Yes, there are handlers
		 * @return false No handlers registered.
		 */
		template<class T> bool hasHandlers() {
			const std::lock_guard<std::recursive_mutex> lock(mutex);
			return Router::hasHandlers<T>();
		}

	protected:
		std::recursive_mutex mutex;
	};
}
