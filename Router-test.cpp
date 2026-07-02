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

#include <optional>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "Router.hpp"

namespace {
	struct Event {
		int value;
	};

	int defaultHandlerCallCount = 0;

	struct EventWithDefault {
		int value;
		static void defaultHandler(const EventWithDefault&) {
			++defaultHandlerCallCount;
		}
	};
}

TEST_CASE("Router invokes a registered handler") {
	rt::Router router;
	int received = 0;
	router.addHandler<Event>([&](const Event& e) { received = e.value; });
	router.handle(Event{42});
	CHECK(received == 42);
}

TEST_CASE("Router invokes handlers in reverse registration order") {
	rt::Router router;
	std::vector<int> order;
	router.addHandler<Event>([&](const Event&) { order.push_back(1); });
	router.addHandler<Event>([&](const Event&) { order.push_back(2); });
	router.addHandler<Event>([&](const Event&) { order.push_back(3); });
	router.handle(Event{0});
	CHECK(order == std::vector<int>{3, 2, 1});
}

TEST_CASE("Router removeHandler removes only the targeted handler") {
	rt::Router router;
	int firstCount = 0;
	int secondCount = 0;
	auto first = router.addHandler<Event>([&](const Event&) { ++firstCount; });
	router.addHandler<Event>([&](const Event&) { ++secondCount; });

	router.removeHandler(first);
	router.handle(Event{0});

	CHECK(firstCount == 0);
	CHECK(secondCount == 1);
}

TEST_CASE("Router hasHandlers reflects registration state") {
	rt::Router router;
	CHECK_FALSE(router.hasHandlers<Event>());

	auto handle = router.addHandler<Event>([](const Event&) {});
	CHECK(router.hasHandlers<Event>());

	router.removeHandler(handle);
	CHECK_FALSE(router.hasHandlers<Event>());
}

TEST_CASE("send() defers mutation during dispatch so it doesn't affect the current call") {
	rt::Router router;
	std::vector<std::string> order;
	std::optional<rt::HandlerReference> handlerB;

	// Registered first, so it runs second (handlers fire in reverse registration order).
	handlerB = router.addHandler<Event>([&](const Event&) { order.push_back("B"); });

	// Registered second, so it runs first. While running, it removes B and adds a new
	// handler C -- neither mutation should affect the dispatch already in progress.
	router.addHandler<Event>([&](const Event&) {
		order.push_back("A");
		router.removeHandler(*handlerB);
		router.addHandler<Event>([&](const Event&) { order.push_back("C"); });
	});

	router.handle(Event{0});
	CHECK(order == std::vector<std::string>{"A", "B"});

	order.clear();
	router.handle(Event{0});
	// B was actually removed, and C was actually added, after the prior dispatch finished.
	// A is still registered, so both A and C (newest first) fire this time.
	CHECK(order == std::vector<std::string>{"C", "A"});
}

TEST_CASE("A handler added and removed within the same dispatch never fires") {
	rt::Router router;
	std::vector<std::string> order;

	router.addHandler<Event>([&](const Event&) {
		order.push_back("A");
		auto c = router.addHandler<Event>([&](const Event&) { order.push_back("C"); });
		router.removeHandler(c);
	});

	router.handle(Event{0});
	CHECK(order == std::vector<std::string>{"A"});

	order.clear();
	router.handle(Event{0});
	// C never actually got added, so only A fires on subsequent calls too.
	CHECK(order == std::vector<std::string>{"A"});
}

TEST_CASE("A handler may reenter handle() for the same event type without corrupting dispatch") {
	rt::Router router;
	std::vector<std::string> order;

	router.addHandler<Event>([&](const Event& e) {
		order.push_back("outer");
		if(e.value == 0) {
			router.handle(Event{1});
		}
	});

	router.handle(Event{0});
	CHECK(order == std::vector<std::string>{"outer", "outer"});
}

TEST_CASE("A type's static defaultHandler is registered automatically") {
	defaultHandlerCallCount = 0;
	rt::Router router;
	CHECK(router.hasHandlers<EventWithDefault>());
	router.handle(EventWithDefault{0});
	CHECK(defaultHandlerCallCount == 1);
}

TEST_CASE("User-registered handlers run alongside a type's default handler") {
	defaultHandlerCallCount = 0;
	rt::Router router;
	int userCount = 0;
	router.addHandler<EventWithDefault>([&](const EventWithDefault&) { ++userCount; });
	router.handle(EventWithDefault{0});
	CHECK(userCount == 1);
	CHECK(defaultHandlerCallCount == 1);
}

TEST_CASE("ThreadSafeRouter behaves like Router for basic add/handle/remove") {
	rt::ThreadSafeRouter router;
	int received = 0;
	auto handle = router.addHandler<Event>([&](const Event& e) { received = e.value; });
	CHECK(router.hasHandlers<Event>());

	router.handle(Event{7});
	CHECK(received == 7);

	router.removeHandler(handle);
	CHECK_FALSE(router.hasHandlers<Event>());
}

TEST_CASE("ThreadSafeRouter allows a handler to reenter the router without deadlocking") {
	rt::ThreadSafeRouter router;
	bool reentered = false;
	router.addHandler<Event>([&](const Event&) {
		// Reentrant call while the dispatch lock is held; this would deadlock with a
		// plain (non-recursive) mutex.
		reentered = router.hasHandlers<Event>();
	});
	router.handle(Event{0});
	CHECK(reentered);
}
