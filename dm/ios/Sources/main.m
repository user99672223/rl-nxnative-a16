// iOS App entry: set the environment (game files, saves, shader libraries), write the host log to Documents/log.txt, create the window (front_ios.m), then run the host's dm_main on a big-stack thread.
// The game files (RomFS) ship inside the App at <App>/romfs; Documents/romfs.img (one image, scripts/push_romfs.sh; its name in any letter case) or a Documents/romfs folder is also accepted (lets you swap them without reinstalling). The DLC folder aoc/ likewise: Documents/aoc, else <App>/aoc.
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "game_profile.h"   // game profile constants (the RomFS marker path)
#include "front.h"
#include "romfs_image.h"
#include "romfs_identity.h"
#include "player_env.h"
#ifndef DM_PLAYER_BUILD
#define DM_PLAYER_BUILD 0
#endif

extern char **environ;

int dm_main(int argc, char **argv);   // dm/host/main.c compiled in with -Dmain=dm_main
UIViewController *front_ios_root(void);
void front_ios_focus_event(int ev);

static char *g_argv[4]; static int g_argc;

// Documents/romfs.img, one RomFS image file (dm/tools/romfs_image.py, scripts/push_romfs.sh): only a complete one counts. Its index is written last and ends the file, so a copy cut off partway is skipped
static int romfs_image_complete(NSString *path) {
    romfs_img *m = romfs_image_open(path.fileSystemRepresentation);
    if (!m) {
        if (errno != ENOENT) fprintf(stderr, "[RED] boot: invalid RomFS image %s (errno=%d); trying the folders\n", path.fileSystemRepresentation, errno);
        return 0;
    }
    romfs_image_close(m); return 1;
}
// A build made with DM_COPY_ROMFS=NO carries <App>/romfs.id (dm/host/romfs_identity.h): only a Documents/romfs.img with that identity is used. No romfs.id (the default build): any complete image
static int romfs_identity_accepts(NSString *img) {
    romfs_id want, got;
    int rc = romfs_id_check([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs.id"].fileSystemRepresentation, img.fileSystemRepresentation, &want, &got);
    if (rc != ROMFS_ID_OK && rc != ROMFS_ID_NONE) fprintf(stderr, "[RED] boot: Documents/%s is not the image this build needs (verdict %d); not used\n", img.lastPathComponent.UTF8String, rc);
    return rc == ROMFS_ID_OK || rc == ROMFS_ID_NONE;
}
// The images in Documents: romfs.img in any letter case, romfs.img itself first. The NSP script writes the image as RomFS.img (extract/<version>/RomFS.img) and the Mac's
// file system ignores letter case, but iOS's does not: a copy made through Finder or the Files app keeps the source's name, which an exact lookup of romfs.img never finds
static NSArray<NSString *> *romfs_document_images(NSString *doc) {
    NSMutableArray<NSString *> *imgs = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:doc error:nil])
        if ([n caseInsensitiveCompare:@"romfs.img"] == NSOrderedSame) [imgs addObject:[doc stringByAppendingPathComponent:n]];
    [imgs sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        BOOL ea = [a.lastPathComponent isEqualToString:@"romfs.img"], eb = [b.lastPathComponent isEqualToString:@"romfs.img"];
        return ea != eb ? (ea ? NSOrderedAscending : NSOrderedDescending) : [a compare:b];
    }];
    return imgs;
}
// The app's three large data files (the game's file system image, the ASTC texture pack, the shader library: 6.2 of the IPA's 6.2 GB) also kept in Documents, so that a later
// build can be installed as a slim IPA of about 40 MB without them (scripts/fetch_ipa.sh --slim): iOS takes only a whole app at an install, and they do not change from build
// to build. A full build makes them at each start as APFS clones of its own files: no time and no storage beyond the app's, and the blocks stay when a slim build replaces
// the app. A file of another size in Documents is from an older full build and is replaced. Never a plain copy (8.7 GB at a launch); a clone that fails is logged and the
// full build runs from its own files as before. Kept out of device backups
#include <sys/clonefile.h>
#include <errno.h>
// iOS keeps an app's bundle and its Documents on different volumes (clonefile: "Cross-device link" on an iPhone 15, iOS 27.2), so a clone is tried and a copy follows:
// on a thread of its own, at low priority, into <name>.seed and renamed when complete, only when the phone has the room plus 2 GB to spare. The game reads the bundle's
// files meanwhile. The copy costs the storage of the data once more until a slim build replaces the full one, whose bundle then no longer holds them
#include <copyfile.h>
static void data_seed_one(NSString *src, NSString *dst) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSDictionary *sa = [fm attributesOfItemAtPath:src error:nil], *da = [fm attributesOfItemAtPath:dst error:nil];
    if (!sa || sa.fileSize < (64u << 20)) return;
    if (da && da.fileSize == sa.fileSize) return;
    [fm createDirectoryAtPath:dst.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *tmp = [dst stringByAppendingString:@".seed"]; [fm removeItemAtPath:tmp error:nil];
    int how = 0;   // 1 clone, 2 copy
    if (!clonefile(src.fileSystemRepresentation, tmp.fileSystemRepresentation, 0)) how = 1;
    else {
        int err = errno;
        NSDictionary *fs = [fm attributesOfFileSystemForPath:dst.stringByDeletingLastPathComponent error:nil];
        unsigned long long room = [fs[NSFileSystemFreeSize] unsignedLongLongValue];
        if (room < sa.fileSize + (2ull << 30)) { fprintf(stderr, "boot: %s not kept in Documents: no clone (%s) and %.1f GB free, %.1f GB needed\n", src.lastPathComponent.UTF8String, strerror(err), room / 1e9, (sa.fileSize + (2ull << 30)) / 1e9); return; }
        double t0 = CFAbsoluteTimeGetCurrent();
        if (copyfile(src.fileSystemRepresentation, tmp.fileSystemRepresentation, NULL, COPYFILE_DATA)) { fprintf(stderr, "boot: %s could not be copied into Documents (%s)\n", src.lastPathComponent.UTF8String, strerror(errno)); [fm removeItemAtPath:tmp error:nil]; return; }
        NSDictionary *ta = [fm attributesOfItemAtPath:tmp error:nil];
        if (ta.fileSize != sa.fileSize) { fprintf(stderr, "boot: %s copy came out %llu of %llu bytes; dropped\n", src.lastPathComponent.UTF8String, ta.fileSize, sa.fileSize); [fm removeItemAtPath:tmp error:nil]; return; }
        fprintf(stderr, "boot: %s copied into Documents in %.1f s\n", src.lastPathComponent.UTF8String, CFAbsoluteTimeGetCurrent() - t0);
        how = 2;
    }
    [fm removeItemAtPath:dst error:nil];
    if (![fm moveItemAtPath:tmp toPath:dst error:nil]) { [fm removeItemAtPath:tmp error:nil]; return; }
    [[NSURL fileURLWithPath:dst] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    fprintf(stderr, "boot: %s kept in Documents for slim updates (%.0f MB, %s)\n", dst.lastPathComponent.UTF8String, sa.fileSize / 1048576.0, how == 1 ? "a clone" : "a copy");
}
static void data_seed(NSString *doc, NSString *res) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([fm fileExistsAtPath:[res stringByAppendingPathComponent:@"romfs.id"]] || [fm fileExistsAtPath:[[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"programs.txt"]]) return;   // upstream's own slim build, and games of several programs: as they were
    NSMutableArray<NSArray<NSString *> *> *pairs = [NSMutableArray array];
    [pairs addObject:@[[res stringByAppendingPathComponent:@"romfs.img"], [doc stringByAppendingPathComponent:@"romfs.img"]]];
    [pairs addObject:@[[[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"bc_astc.pack"], [doc stringByAppendingPathComponent:@"bc_astc.pack"]]];
    NSString *msl = [res stringByAppendingPathComponent:@"msl"];
    for (NSString *n in [fm contentsOfDirectoryAtPath:msl error:nil]) if ([n hasSuffix:@".img"]) [pairs addObject:@[[msl stringByAppendingPathComponent:n], [[doc stringByAppendingPathComponent:@"msl"] stringByAppendingPathComponent:n]]];
    // in the background: a copy of 6 GB must not hold up the start, and the game reads the bundle's files until the next (slim) install. User-initiated, not utility: an iPhone 15
    // copied the first two files (10 s each) and never finished the 3.6 GB shader library at utility priority while the game ran (its I/O is throttled)
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ for (NSArray<NSString *> *p in pairs) @autoreleasepool { data_seed_one(p[0], p[1]); } });
}
static NSString *romfs_locate(NSString *prog) {   // prog: a game of several programs, the running program's folder in the app (p<N>); nil otherwise
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    if (prog) {   // the program's files are in the app (Documents holds one image, one program's)
        NSString *p = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:prog], *img = [p stringByAppendingPathComponent:@"romfs.img"], *d = [p stringByAppendingPathComponent:@"romfs"];
        if (romfs_image_complete(img)) return img;
        return [fm fileExistsAtPath:[d stringByAppendingPathComponent:@(DM_GAME_ROMFS_MARKER)]] ? d : nil;
    }
    for (NSString *img in romfs_document_images(doc)) {
        if (!romfs_image_complete(img) || !romfs_identity_accepts(img)) continue;
        // A folder copy from before images is now dead weight of up to many GB (often cut off partway). Deleting its 300,000 files here took longer than iOS allows an app to start
        // (Tears of the Kingdom: killed on a black screen), so it is moved aside at once and deleted in the background, leftovers of an interrupted delete included
        NSString *old = [doc stringByAppendingPathComponent:@"romfs"], *tmp = NSTemporaryDirectory();
        if ([fm fileExistsAtPath:old] && [fm moveItemAtPath:old toPath:[tmp stringByAppendingPathComponent:[@"romfs-old-" stringByAppendingString:NSUUID.UUID.UUIDString]] error:nil])
            fprintf(stderr, "boot: Documents/romfs moved aside for deletion (Documents/%s replaces it)\n", img.lastPathComponent.UTF8String);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
            NSFileManager *bfm = [NSFileManager new];
            for (NSString *n in [bfm contentsOfDirectoryAtPath:tmp error:nil]) if ([n hasPrefix:@"romfs-old-"]) [bfm removeItemAtPath:[tmp stringByAppendingPathComponent:n] error:nil];
        });
        return img;
    }
    NSString *bundledImage = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs.img"];
    NSString *documentFolder = [doc stringByAppendingPathComponent:@"romfs"];
    if ([fm fileExistsAtPath:[documentFolder stringByAppendingPathComponent:@(DM_GAME_ROMFS_MARKER)]]) return documentFolder;
    if (romfs_image_complete(bundledImage)) return bundledImage;
    for (NSString *d in @[[doc stringByAppendingPathComponent:@"romfs"], [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs"]])
        if ([fm fileExistsAtPath:[d stringByAppendingPathComponent:@(DM_GAME_ROMFS_MARKER)]]) return d;   // only a directory containing the game profile's romfs_marker path counts (Mario: System/Resource, the resource size table)
    return nil;
}

// Add-on content (DLC): each DLC's RomFS image at <dir>/<index>.img (or a legacy <index>/ folder). Like the game files, Documents/aoc comes before <App>/aoc.
// A DM_AOC_DIR that is already set (env.txt, the launch environment) wins. The host mounts and counts the DLC from DM_AOC_DIR (hle_fs.c, hle_misc.c)
static void aoc_locate(void) {
    if (getenv("DM_AOC_DIR")) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    for (NSString *d in @[[doc stringByAppendingPathComponent:@"aoc"], [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aoc"]]) {
        BOOL dir = NO;
        if (![fm fileExistsAtPath:d isDirectory:&dir] || !dir) continue;
        setenv("DM_AOC_DIR", d.fileSystemRepresentation, 0);
        fprintf(stderr, "boot: DLC from %s\n", d.fileSystemRepresentation);
        return;
    }
}

// Boot prerequisites (done before UIApplicationMain): directories / environment variables / log -- the host's early load reads these
// Why a slim build found no usable game files: the verdict on Documents/romfs.img (the first of romfs_document_images), in the log and for the screen (front_ios_boot_message). NULL when the build has no romfs.id
static char g_boot_message[640];
const char *front_ios_boot_message(void) { return g_boot_message[0] ? g_boot_message : NULL; }
// Why this device cannot run the games: its GPU has no BC texture formats, which the games' textures use and the host does not decompress (dm/gfx/tex/README.md; the A16 and older,
// GitHub issue #704), so the screen says so instead of Metal aborting at the game's first BC texture. NULL on a supported device. Called by startGame (front_ios.m) once the launcher has set
// its switches: this makes the process's first Metal device, and Metal reads MTL_HUD_ENABLED then; checked at launch, it kept the launcher's Performance HUD off (GitHub issue #840)
const char *front_ios_device_check(void) {
    static char msg[320]; static int done;
    if (done++) return msg[0] ? msg : NULL;
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();   // the system default device, the one the graphics layers take (mtl_init repeats this check for the other routes)
    if (gpu.supportsBCTextureCompression) return NULL;
    // A GPU without BC formats runs a game packed with its BC textures re-encoded as ASTC (dm/bc_astc.pack, made by dm/tools/ue3_astc.py; nvn_mtl.m takes it through
    // DM_BCSUB_PACK, set at boot below): an A16 iPhone, the device Rocket League's port is for
    if (getenv("DM_BCSUB_PACK")) { fprintf(stderr, "boot: this GPU (%s) has no BC texture formats; the app's ASTC pack stands in for them\n", gpu ? gpu.name.UTF8String : "no Metal device"); return NULL; }
    snprintf(msg, sizeof msg, "This device's GPU (%s) cannot read BC-compressed textures, which the games use. It needs an iPhone 15 Pro or newer (A17 Pro chip or later) or an iPad with an M1 chip or later.",
             gpu ? gpu.name.UTF8String : "no Metal device");
    fprintf(stderr, "[RED] boot: %s\n", msg);
    return msg;
}
static void romfs_identity_report(NSString *doc) {
    romfs_id want, got;
    int rc = romfs_id_check([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs.id"].fileSystemRepresentation,
                            (romfs_document_images(doc).firstObject ?: [doc stringByAppendingPathComponent:@"romfs.img"]).fileSystemRepresentation, &want, &got);
    if (rc == ROMFS_ID_OK || rc == ROMFS_ID_NONE) return;
    romfs_id_message(rc, &want, &got, NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "<bundle ID>", g_boot_message, sizeof g_boot_message);
    fprintf(stderr, "[RED] boot: %s\n", g_boot_message);
}

static NSString *program_env(void);
static int boot_env(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    NSString *res = NSBundle.mainBundle.resourcePath;
    for (NSString *d in @[@"save", @"temp"]) [fm createDirectoryAtPath:[doc stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
    // Keep game files and scratch data out of device backups (Documents is backed up by default; a RomFS placed there is many GB). Saves, logs and the pipeline list stay in the backup
    for (NSString *d in [@[@"romfs", @"aoc", @"temp"] arrayByAddingObjectsFromArray:[romfs_document_images(doc) valueForKey:@"lastPathComponent"]]) {
        NSURL *u = [NSURL fileURLWithPath:[doc stringByAppendingPathComponent:d]]; NSError *e = nil;
        if ([fm fileExistsAtPath:u.path] && ![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e]) fprintf(stderr, "boot: %s could not be excluded from backup: %s\n", d.UTF8String, e.localizedDescription.UTF8String);
    }
    // Log: the host only writes to stderr; redirect it to Documents/log.txt (the previous one is kept as log.prev.txt), retrieve it from the Files app when something goes wrong
    NSString *log = [doc stringByAppendingPathComponent:@"log.txt"], *prev = [doc stringByAppendingPathComponent:@"log.prev.txt"];
    [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:log toPath:prev error:nil];
    if (freopen(log.fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    // Environment switch files (for on-device diagnostics: a sideloaded app cannot be given environment variables from outside): one KEY=VALUE per line, # starts a comment; Documents/env.txt takes precedence over dm/env.txt in the bundle (put there by build_ipa.sh's DM_ENV_FILE)
    NSMutableArray<NSString *> *from_file = [NSMutableArray array], *from_launch = [NSMutableArray array];
    for (char **e = environ; *e; e++) {   // set at launch (Xcode, devicectl): before the env files, and the launcher keeps them
        const char *eq = strchr(*e, '='); NSString *k = eq && !strncmp(*e, "DM_", 3) ? [[NSString alloc] initWithBytes:*e length:(NSUInteger)(eq - *e) encoding:NSUTF8StringEncoding] : nil;
        if (k) [from_launch addObject:k];
    }
#if DM_PLAYER_BUILD
    // Player-facing choices belong to the launcher, including saved choices.
    // Neither old diagnostic files nor bundled defaults may lock those rows.
    NSMutableSet<NSString *> *launcher_keys = [NSMutableSet set];
    NSData *settings_data = [NSData dataWithContentsOfFile:[res stringByAppendingPathComponent:@"dm/launcher.json"]];
    NSDictionary *settings_file = settings_data ? [NSJSONSerialization JSONObjectWithData:settings_data options:0 error:nil] : nil;
    for (NSDictionary *row in settings_file[@"settings"]) if ([row[@"env"] isKindOfClass:NSString.class]) [launcher_keys addObject:row[@"env"]];
#endif
    for (NSString *ef in @[[doc stringByAppendingPathComponent:@"env.txt"], [[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"env.txt"]]) {
        NSString *txt = [NSString stringWithContentsOfFile:ef encoding:NSUTF8StringEncoding error:nil];
        for (NSString *raw in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
            NSString *ln = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; NSRange eq = [ln rangeOfString:@"="];
            if (!ln.length || [ln hasPrefix:@"#"] || eq.location == NSNotFound || !eq.location) continue;
            NSString *k = [ln substringToIndex:eq.location], *v = [ln substringFromIndex:eq.location + 1];
#if DM_PLAYER_BUILD
            if (dm_player_diagnostic_key(k.UTF8String) || [launcher_keys containsObject:k] ||
                ([launcher_keys containsObject:@"DM_METAL_HUD"] && [k isEqualToString:@"MTL_HUD_ENABLED"])) continue;
#endif
            if (!getenv(k.UTF8String)) { setenv(k.UTF8String, v.UTF8String, 0); [from_file addObject:k]; fprintf(stderr, "boot: env %s=%s (%s)\n", k.UTF8String, v.UTF8String, ef.lastPathComponent.UTF8String); }
        }
    }
    setenv("DM_ENV_FILE_KEYS", [from_file componentsJoinedByString:@" "].UTF8String, 1);   // the launcher keeps what an env file set (LauncherSettings.swift)
    setenv("DM_ENV_LAUNCH_KEYS", [from_launch componentsJoinedByString:@" "].UTF8String, 1);   // and what the launch environment set (docs/frontends.md, Saved choices)
    // The memory watcher (memstat.m): its constructor starts it only when NX_MEMORY_BUDGET_MB is in the launch environment, which a sideloaded app never has. Without this
    // call the first device log had 0 MiB in every row, and a budget in env.txt was never read
    { extern void dm_mem_budget_start(void); dm_mem_budget_start(); }
    // Rocket League test builds (2026-11-14): one frame's passes into the log every 45 s, for the scene drawn into part of a 1440p screen in matches (docs/optimizations.md
    // item 15), which only a device reaches. An env file's or the launch environment's DM_NVN_PASSDUMP wins; DM_NVN_PASSDUMP=off there turns it off. Remove once that is fixed
    if (!getenv("DM_NVN_PASSDUMP")) setenv("DM_NVN_PASSDUMP", "every:2700", 0);
    else if (!strcmp(getenv("DM_NVN_PASSDUMP"), "off")) unsetenv("DM_NVN_PASSDUMP");
    front_hud_env();   // an env file's DM_METAL_HUD; nothing in this process makes a Metal device before startGame (front_ios.m), which applies the launcher's
    // Game language (nn::oe::GetDesiredLanguage, hle_oe.c): the one the IPA was packed with (the NSP script's LANGUAGE / --language → DMLanguage in Info.plist), unless an env file sets DM_LANG
    // (or the launcher, which replaces this default before the game starts)
    NSString *lang = [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMLanguage"];
    if (!getenv("DM_LANG") && [lang isKindOfClass:NSString.class] && lang.length) setenv("DM_LANG", lang.UTF8String, 0);
    fprintf(stderr, "boot: game language %s\n", getenv("DM_LANG") ?: "en-US");
    data_seed(doc, res);
    NSString *dm = program_env();
    if (!dm) { romfs_identity_report(doc); fprintf(stderr, "boot: game files not found (Documents/romfs.img, Documents/romfs or <App>/romfs)\n"); return 0; }
    setenv("DM_SAVE", [doc stringByAppendingPathComponent:@"save"].fileSystemRepresentation, 0);
    setenv("DM_TEMP", [doc stringByAppendingPathComponent:@"temp"].fileSystemRepresentation, 0);
    setenv("DM_GUEST_FILE_DIR", NSTemporaryDirectory().fileSystemRepresentation, 0);   // DM_GUEST_FILE (hle_svc.c) keeps the guest's memory in files there
    setenv("DM_FONT_DIR", [[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"font"].fileSystemRepresentation, 0);   // Shared-font stand-ins (standard.ttf / nintendo_ext.ttf): iOS lacks the macOS font path
    aoc_locate();   // add-on content: Documents/aoc, else <App>/aoc; an empty Documents/aoc explicitly selects no DLC
    NSString *pso = [res stringByAppendingPathComponent:@"pso"];   // the pipeline archive made offline (build_ipa.sh, dm/gfx/tools/pso_offline.py): the shader pairs' bases, which the device then does not build
    if ([fm fileExistsAtPath:pso]) setenv("DM_PSO_BUNDLE", pso.fileSystemRepresentation, 0);
    {   // the ASTC stand-ins of the game's BC textures, for a GPU without BC formats and for NX_FORCE_ASTC_ONLY (nvn_mtl.m bcs_pf): in the app, or put into Documents beside the log
        NSString *pack = [[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"bc_astc.pack"], *docpack = [doc stringByAppendingPathComponent:@"bc_astc.pack"];
        if ([fm fileExistsAtPath:docpack]) setenv("DM_BCSUB_PACK", docpack.fileSystemRepresentation, 0);
        else if ([fm fileExistsAtPath:pack]) setenv("DM_BCSUB_PACK", pack.fileSystemRepresentation, 0);
    }
    chdir(doc.fileSystemRepresentation);
    g_argv[0] = strdup("dmhost"); g_argv[1] = strdup(dm.fileSystemRepresentation); g_argc = 2;
    dm_host_preload_start(g_argc, g_argv);   // Module tables / relocation / takeover run early in the background, in parallel with UIKit startup (and the launcher: the settings it sets are read when the game starts)
    return 1;
}

// The running program's files (an app holding the programs of one game, build_ipa.sh PROGRAMS_TSV: Super Mario 3D All-Stars' launcher and the games it starts): dm/programs.txt lists them, and each has
// its own: <App>/p<N>/romfs.img (html and jit beside it), <App>/msl/p<N>, <App>/dm/p<N> (rewriter tables, shader tables, launcher settings) and its modules in sections tagged p<N> (DM_EMBED_TAG, loader.c).
// The host's profile is the program's (DM_PROGRAM, main.c dm_game_cur). oe::ExecuteProgram records the program and its parameters in Documents/program.bin (DM_EXEC_OUT, hle_oe.c), names it on screen, schedules a
// notification that opens the app (front_ios.m front_program_handoff) and ends; this start reads the record and takes it away, so a later start, after the player quit the game, is at the launcher as on the Switch.
// Started from the record, the settings screen stays away (DM_LAUNCHER=0): the player is on the way into a game. Returns the program's dm folder (the host's modules folder), nil when its game files are missing
static NSString *program_env(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0], *res = NSBundle.mainBundle.resourcePath;
    NSString *dmdir = [res stringByAppendingPathComponent:@"dm"], *msl = [res stringByAppendingPathComponent:@"msl"], *prog = nil;
    NSString *plist = [NSString stringWithContentsOfFile:[dmdir stringByAppendingPathComponent:@"programs.txt"] encoding:NSUTF8StringEncoding error:nil];
    if (plist) {
        NSArray *have = [plist componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString *rec = [doc stringByAppendingPathComponent:@"program.bin"]; NSData *d = [NSData dataWithContentsOfFile:rec]; int32_t n = 0;
        if (d.length >= 4) { memcpy(&n, d.bytes, 4); [fm removeItemAtPath:rec error:nil]; setenv("DM_LAUNCHER", "0", 1); }
        if (![have containsObject:[NSString stringWithFormat:@"%d", n]]) { fprintf(stderr, "[RED] boot: program %d is not in this app; program 0\n", n); n = 0; d = nil; }
        prog = [NSString stringWithFormat:@"p%d", n];
        NSString *lp = [doc stringByAppendingPathComponent:@"launch_param.bin"];
        if (d.length > 4) { [[d subdataWithRange:NSMakeRange(4, d.length - 4)] writeToFile:lp atomically:YES]; setenv("DM_LAUNCH_PARAM", lp.fileSystemRepresentation, 1); }
        else { [fm removeItemAtPath:lp error:nil]; unsetenv("DM_LAUNCH_PARAM"); }
        setenv("DM_PROGRAM", [NSString stringWithFormat:@"%d", n].UTF8String, 1);
        setenv("DM_EMBED_TAG", prog.UTF8String, 1);
        setenv("DM_EXEC_OUT", rec.fileSystemRepresentation, 0);
        dmdir = [dmdir stringByAppendingPathComponent:prog]; msl = [msl stringByAppendingPathComponent:prog];
        setenv("DM_APP_DM", dmdir.fileSystemRepresentation, 1);   // the launcher's settings (Launcher.swift)
        fprintf(stderr, "boot: program %d of %s (%zu bytes of launch parameter)\n", n, [[plist stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String, d.length > 4 ? (size_t)d.length - 4 : 0);
    }
    if (!prog && ![fm contentsOfDirectoryAtPath:msl error:nil].count) {   // a slim build: the shader library a full build kept in Documents (data_seed)
        NSString *dmsl = [doc stringByAppendingPathComponent:@"msl"];
        if ([fm contentsOfDirectoryAtPath:dmsl error:nil].count) { msl = dmsl; fprintf(stderr, "boot: shader library from Documents/msl (slim build)\n"); }
        else fprintf(stderr, "[RED] boot: this is a slim build and Documents has no shader library: install a full build once and start it\n");
    }
    NSString *romfs = romfs_locate(prog);
    if (!romfs) return nil;
    fprintf(stderr, "boot: game files %s\n", romfs.fileSystemRepresentation);
    setenv("DM_ROMFS", romfs.fileSystemRepresentation, 0);
    setenv("DM_NVN_SHMAP", [dmdir stringByAppendingPathComponent:@"shader_map.tsv"].fileSystemRepresentation, 0);
    setenv("DM_MSL_DIR", msl.fileSystemRepresentation, 0);
    return dmdir;
}
static int g_boot_ok;

static void *boot_thread(void *p) {
    (void)p;
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);   // crash printing must work on the host thread too
    int rc = dm_main(g_argc, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}
void front_ios_boot(void) {   // Called by front_ios.m once the window and Metal view exist
    static int once; if (once++ || !g_boot_ok) return;
    void front_ios_audio_session(void); front_ios_audio_session();   // set a short I/O buffer before the game opens audio output (front_ios.m)
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);   // the Switch main thread stack is set by npdm; the host gives plenty
    pthread_t t; pthread_create(&t, &a, boot_thread, NULL); pthread_detach(t);
}
int front_ios_boot_ok(void) { return g_boot_ok; }

@interface App : UIResponder <UIApplicationDelegate>
@end
@implementation App
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    return [[UISceneConfiguration alloc] initWithName:[s.role isEqualToString:UIWindowSceneSessionRoleExternalDisplayNonInteractive] ? @"TV" : @"Default" sessionRole:s.role];   // a TV (front_ios.m DmTVSceneDelegate)
}
@end

// Scene lifecycle: create the window + hand focus events to front_ios.m
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = front_ios_root();
    [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)s { front_ios_focus_event(0); }
- (void)sceneDidEnterBackground:(UIScene *)s { front_ios_focus_event(1); }
- (void)sceneWillEnterForeground:(UIScene *)s { front_ios_focus_event(2); }
- (void)sceneDidBecomeActive:(UIScene *)s { front_ios_focus_event(3); }
@end

int main(int argc, char **argv) {
    @autoreleasepool { g_boot_ok = boot_env(); }
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class)); }
}
