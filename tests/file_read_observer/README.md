# Read-only file observation

`file_read_observer.h` supplies an optional synchronous host callback for
`xbox_NtReadFile`. It receives the original native handle, status, requested and
completed lengths, and borrowed bytes. Failure/empty/out-of-bounds observations
expose no bytes. Nested reads execute normally without recursive notifications.
Successful completion events are signaled after the callback returns. Installing
or replacing the callback requires quiescent I/O, normally before guest startup.

This is host instrumentation. It cannot override a read result and is not a
sandbox, a file-content stability guarantee, or a game/save identity assertion.
Keep callbacks bounded, never retain/use the borrowed handle or buffer after
return, and do not wait on external work.

The default standalone check needs only a C compiler:

```sh
cmake -S tests/file_read_observer -B build-observer
cmake --build build-observer --config Release
ctest --test-dir build-observer -C Release --output-on-failure
```

`-DFILE_READ_OBSERVER_IO_TEST=ON` also tests real kernel reads, short reads, EOF,
invalid handles, missing status blocks, nested reads and completion-event order.
Linux and MSVC Windows use focused platform/kernel links without SDL or title
assets. Adding this test directory to a project that already provides the
`xboxrecomp` target tests the full-runtime link instead. Both CI jobs cover
actual kernel I/O as well as dispatch. Other Windows toolchains can use the
full-runtime configuration with its SDL dependency.
