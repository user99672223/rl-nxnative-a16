// The null GPU: stand-ins for the Metal 4 objects on a device without Metal 4 (a virtual Mac's paravirtualized GPU, such as GitHub's macOS runners and the iOS Simulator on them).
// DM_NVN_NULL=1 selects it when mtl_init finds no Metal 4; a device with Metal 4 never uses it. The replay layer then runs as it is (passes, pipeline keys, draws, uploads on the classic
// queue), but nothing it encodes for the Metal 4 queue is executed: every Metal 4 object is one object that answers any message with zero (or itself, where an object is expected), a
// commit calls its feedback handlers at once in commit order, and a signalled event is raised from the CPU in the same order. Enough to boot a game, drive it and count what it would
// draw; no picture, no GPU time. Why it exists: Rocket League's bring-up had only such machines to run on (PROGRESS.md of that port).
#pragma once
#import <Metal/Metal.h>

extern int g_nvn_null;                       // 1 once mtl_init took the null GPU
id dm_null_obj(void);                        // the one stand-in object
MTL4CommitOptions *dm_null_commit_options(void);   // what a commit's feedback handlers are added to in null mode
// A device for the calls only a Metal 4 device answers (command allocators, Metal 4 command buffers, argument tables, compilers, archives, counter heaps, residency sets)
#define DEV4 ((id<MTLDevice>)(g_nvn_null ? dm_null_obj() : (id)g_dev))
#define DM_COMMIT_OPTIONS_NEW() (g_nvn_null ? dm_null_commit_options() : [MTL4CommitOptions new])
