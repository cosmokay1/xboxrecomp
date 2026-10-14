Linux-only regression including the actual CPU pushbuffer renderer. Synthetic
mmap guest memory tests textured alpha blending and deferred dirty-row flush,
protected/cross-page textures and surfaces, read-only output failure, and vertex
inline/stride bounds. The production guest-copy helper handles page failures;
no signal handler or fake rendering guard is installed. Unused runtime and SDL
paths are discarded through function/data section garbage collection. No window,
retail assets, GPU rendering, game hooks or gameplay are exercised.

    cmake -S external/xboxrecomp/tests/renderer_staging -B /tmp/renderer-staging -DCMAKE_PREFIX_PATH=/path/to/SDL3
    cmake --build /tmp/renderer-staging
    ctest --test-dir /tmp/renderer-staging --output-on-failure

The swizzle/DXT helpers are the actual header implementations included by the
renderer. This test does not claim Linux execution from macOS.
