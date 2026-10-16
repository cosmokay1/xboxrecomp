#pragma once
/* Private experimental local-process transport. Windows mutex protects all
 * fields; the renderer never waits for it. Not a public addon ABI. */
#define SHADOW_LIVE_MAGIC 0x53425831u
#define SHADOW_LIVE_PIXELS (256u*256u)
typedef struct ShadowLiveFrame {
    unsigned int magic,sequence;
    float light[3];
    unsigned long long tick;
    unsigned int mask[SHADOW_LIVE_PIXELS];
} ShadowLiveFrame;
