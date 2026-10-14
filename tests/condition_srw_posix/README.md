# POSIX condition-variable regression

This standalone test builds the actual compatibility layer, without SDL or game
assets. It covers immediate wake/wake-all in shared and exclusive mode, timeout,
reacquisition, invalid flags, and existing critical-section timeout behavior.

```
cmake -S tests/condition_srw_posix -B build-condition
cmake --build build-condition
ctest --test-dir build-condition --output-on-failure
```

The wait releases the SRW lock and enters its condition wait without a lost-wake
interval, then reacquires the original lock mode. Timeout returns FALSE with
ERROR_TIMEOUT, following [Microsoft's API contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw).
Spurious/stolen wakeups still require the caller's predicate loop.

POSIX SwitchToThread invokes sched_yield and returns FALSE conservatively:
POSIX does not report whether another thread actually ran. This avoids claiming
an observed handoff, which [the Windows return value represents](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-switchtothread).
The Windows implementation continues to use its native API. These tests do not
verify original game execution, guest scheduling under gameplay or rendering.
