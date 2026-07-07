# Router.hpp

[API docs](https://lrmoorejr.github.io/router/)

A small, type-safe pub/sub event dispatcher for C++20. Register a handler for an event class
with `addHandler()`, then broadcast events of that type with `handle()` -- no base class, no
enum of event kinds, no manual `dynamic_cast`. Handlers are matched by `std::type_index`, so any
type can be an event.

I originally wrote this library for an experimental GUI system.  The GUI uses Router for all of its 
event handling system, but eventually I realized that I wanted to use Router in other places, 
so I extracted it as its own single-header library.  Router seems pretty quick, with dispatches in 
the range of 11ns on my laptop.

```cpp
#include <cstdio>
#include "Router.hpp"

struct PlayerScored { int playerId; int points; };

rt::Router router;

auto handle = router.addHandler<PlayerScored>([](const PlayerScored& e) {
    printf("player %d scored %d\n", e.playerId, e.points);
});

router.handle(PlayerScored{.playerId = 3, .points = 10});

router.removeHandler(handle);
```

## Requirements

- C++20 or later
- RTTI enabled (Router is keyed on `std::type_index`/`typeid`; it will not work with
  `-fno-rtti`)
- Header-only -- copy `Router.hpp` into your project and `#include` it
- Optional: [`Ensure.hpp`](https://github.com/lrmoorejr/ensure) for a formatted diagnostic on a
  usage error; if it's not present, Router falls back to plain `assert()` (see below)

## API

| Call | Behavior |
|---|---|
| `addHandler<T>(std::function<void(const T&)>)` | Registers a handler for event type `T`. Returns a `HandlerReference` for use with `removeHandler()`. |
| `handle<T>(const T& event)` | Invokes every handler registered for `T`, most-recently-added first. |
| `removeHandler(HandlerReference)` | Unregisters a previously-added handler. |
| `hasHandlers<T>()` | Returns whether any handler is currently registered for `T`. |

### Default handlers

If an event type `T` defines a `static void defaultHandler(const T&)`, Router registers it
automatically the first time `T` is seen -- before any handler you add explicitly. This is handy
for a fallback (e.g. logging unhandled events) without every call site having to remember to wire
one up.

```cpp
#include <iostream>
#include <string_view>

struct Unhandled {
    std::string_view what;
    static void defaultHandler(const Unhandled& e) {
        std::cerr << "unhandled: " << e.what << "\n";
    }
};
```

### Thread safety

`Router` itself is not thread-safe. `ThreadSafeRouter` is a drop-in replacement that adds a
mutex around the same API. The mutex is recursive, so a handler invoked from `handle()` may
safely call back into `addHandler()`, `removeHandler()`, or `handle()` on the same router
without deadlocking.

```cpp
rt::ThreadSafeRouter router;
```

### Reentrancy

Adding or removing a handler from within a handler that's currently running is safe and well
defined: the mutation is deferred until the outermost `handle()` call for that event type
finishes, so it never affects the dispatch already in progress. A handler added mid-dispatch
won't fire until the next `handle()` call; a handler removed mid-dispatch still fires for the
remainder of the current call.

Because mutations are only deferred (not applied to the live handler list) when a dispatch is
actually in progress, the common case -- calling `handle()` when nothing is being added or
removed -- never has to copy the handler list to dispatch safely.

## Ensure.hpp fallback

Router uses one runtime check (in `removeHandler`, to catch removing a handler for a type that
was never registered). If [`Ensure.hpp`](https://github.com/lrmoorejr/ensure) is available --
either checked out alongside `Router.hpp`, or reachable as `commons/Ensure.hpp` -- Router uses
its `ensure()` for a formatted diagnostic. Otherwise it falls back to plain `assert()`. Either
way, this check compiles out entirely in release (`NDEBUG`) builds, matching `assert()`'s usual
behavior.

## License

Apache License 2.0 -- see [LICENSE](LICENSE).
