// The C and Objective-C functions Launcher.swift and GameMenu.swift call (CloudSaves.h, DmGameMenu.h, save_sync.h's report before Play). project.yml points SWIFT_OBJC_BRIDGING_HEADER here.
#import "CloudSaves.h"
#import "DmGameMenu.h"
#include "save_sync.h"
// DiagPanel.swift: the host's performance record and memory figures
#include "dm_perf.h"
void dm_mem_now(uint64_t *use, uint64_t *peak, uint64_t *budget, uint64_t *avail, uint64_t *metal);
