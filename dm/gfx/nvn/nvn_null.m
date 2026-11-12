// The null GPU (nvn_null.h): one object that stands in for every Metal 4 object on a device without Metal 4.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <os/lock.h>
#include <stdatomic.h>
#include "nvn_null.h"
#include "../../host/dm.h"

int g_nvn_null;

#define NLOG(...) dm_log("[dm] [nvn-null] " __VA_ARGS__)

// Feedback handlers of one commit (what MTL4CommitOptions holds and gives no way to read back)
@interface DmNullCommitOptions : NSObject { @public NSMutableArray *handlers; }
@end
@implementation DmNullCommitOptions
- (instancetype)init { if ((self = [super init])) handlers = [NSMutableArray array]; return self; }
- (void)addFeedbackHandler:(void (^)(id))h { if (h) [handlers addObject:[h copy]]; }
@end

@interface DmNull : NSObject
@end

static DmNull *g_null;
static dispatch_queue_t g_nq;              // completion order: feedback handlers and event signals run here, one after another, as the GPU would report them
static os_unfair_lock g_sig_lk = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, NSMethodSignature *> *g_sigs;
static _Atomic unsigned long long g_null_commits, g_null_msgs;

// The type encoding of a selector, from the Metal protocols: the ones named here (so their metadata is in this binary whatever the system's drivers register), then every
// registered protocol whose name starts with MTL or CAMetal
static const char *sel_types(SEL s) {
    static Protocol *__unsafe_unretained named[32]; static int nn;
    static Protocol *__unsafe_unretained *all; static unsigned nall;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        named[nn++] = @protocol(MTL4CommandQueue); named[nn++] = @protocol(MTL4CommandBuffer); named[nn++] = @protocol(MTL4CommandAllocator);
        named[nn++] = @protocol(MTL4CommandEncoder); named[nn++] = @protocol(MTL4RenderCommandEncoder); named[nn++] = @protocol(MTL4ComputeCommandEncoder);
        named[nn++] = @protocol(MTL4ArgumentTable); named[nn++] = @protocol(MTL4Compiler); named[nn++] = @protocol(MTL4Archive); named[nn++] = @protocol(MTL4CommitFeedback);
        named[nn++] = @protocol(MTL4CounterHeap); named[nn++] = @protocol(MTL4PipelineDataSetSerializer);
        named[nn++] = @protocol(MTLResidencySet); named[nn++] = @protocol(MTLRenderPipelineState); named[nn++] = @protocol(MTLComputePipelineState);
        named[nn++] = @protocol(MTLLibrary); named[nn++] = @protocol(MTLFunction); named[nn++] = @protocol(MTLDevice);
        all = objc_copyProtocolList(&nall);
    });
    for (int pass = 0; pass < 2; pass++) {
        unsigned n = pass ? nall : (unsigned)nn;
        for (unsigned i = 0; i < n; i++) {
            Protocol *p = pass ? all[i] : named[i];
            if (pass) { const char *nm = protocol_getName(p); if (strncmp(nm, "MTL", 3) && strncmp(nm, "CAMetal", 7)) continue; }
            for (int req = 1; req >= 0; req--) { struct objc_method_description d = protocol_getMethodDescription(p, s, req, YES); if (d.types) return d.types; }
        }
    }
    return NULL;
}

// Object-valued properties that mean "something went wrong" or name something when they are not nil
static int null_returns_nil(SEL s) {
    static const char *const nil_sels[] = {"error", "label", "name", "device", "logs", "functionNames", "installName", "localizedDescription", "underlyingErrors", NULL};
    const char *n = sel_getName(s);
    for (int i = 0; nil_sels[i]; i++) if (!strcmp(n, nil_sels[i])) return 1;
    return 0;
}

@implementation DmNull
- (BOOL)respondsToSelector:(SEL)s { (void)s; return YES; }
- (BOOL)conformsToProtocol:(Protocol *)p { (void)p; return YES; }

// ---- what the null GPU does itself ----
static void null_commit(MTL4CommitOptions *o) {
    atomic_fetch_add(&g_null_commits, 1);
    if (![(id)o isKindOfClass:[DmNullCommitOptions class]]) return;
    NSArray *hs = [((DmNullCommitOptions *)(id)o)->handlers copy];
    if (hs.count) dispatch_async(g_nq, ^{ for (void (^h)(id) in hs) h(g_null); });
}
- (void)commit:(const id<MTL4CommandBuffer> _Nonnull[_Nonnull])cbs count:(NSUInteger)n { (void)cbs; (void)n; atomic_fetch_add(&g_null_commits, 1); }
- (void)commit:(const id<MTL4CommandBuffer> _Nonnull[_Nonnull])cbs count:(NSUInteger)n options:(MTL4CommitOptions *)o { (void)cbs; (void)n; null_commit(o); }
// An event signalled on the queue is raised once everything committed before it has reported; only a shared event has a value the CPU can set
- (void)signalEvent:(id<MTLEvent>)ev value:(uint64_t)v {
    if (![(id)ev respondsToSelector:@selector(setSignaledValue:)] || (id)ev == (id)g_null) return;
    dispatch_async(g_nq, ^{ id<MTLSharedEvent> e = (id<MTLSharedEvent>)ev; if (e.signaledValue < v) e.signaledValue = v; });
}
- (void)waitForEvent:(id<MTLEvent>)ev value:(uint64_t)v { (void)ev; (void)v; }
- (double)GPUStartTime { return CACurrentMediaTime(); }
- (double)GPUEndTime { return CACurrentMediaTime(); }
- (NSString *)description { return @"<DmNull: the null GPU's stand-in>"; }

// ---- everything else: zero, or itself where an object is expected ----
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s {
    NSString *k = NSStringFromSelector(s);
    os_unfair_lock_lock(&g_sig_lk);
    NSMethodSignature *m = g_sigs[k];
    os_unfair_lock_unlock(&g_sig_lk);
    if (m) return m;
    const char *t = sel_types(s);
    if (!t) {
        // Unknown to every Metal protocol: answer as a method of that many object arguments returning nothing, and say so once
        NSUInteger nargs = [[k componentsSeparatedByString:@":"] count] - 1; NSMutableString *ts = [NSMutableString stringWithString:@"v@:"];
        for (NSUInteger i = 0; i < nargs; i++) [ts appendString:@"@"];
        NLOG("[RED] null GPU: a message no Metal protocol declares, answered with nothing: %s", sel_getName(s));
        m = [NSMethodSignature signatureWithObjCTypes:ts.UTF8String];
    } else m = [NSMethodSignature signatureWithObjCTypes:t];
    os_unfair_lock_lock(&g_sig_lk); g_sigs[k] = m; os_unfair_lock_unlock(&g_sig_lk);
    return m;
}
- (void)forwardInvocation:(NSInvocation *)inv {
    atomic_fetch_add(&g_null_msgs, 1);
    NSMethodSignature *m = inv.methodSignature; SEL s = inv.selector; const char *name = sel_getName(s);
    // An asynchronous build's completion handler (a pipeline, a library, a function): called with the stand-in and no error, off the caller's thread as Metal does
    NSUInteger na = m.numberOfArguments;
    if (na > 2 && !strcmp([m getArgumentTypeAtIndex:na - 1], "@?")) {
        size_t ln = strlen(name);
        if (ln >= 18 && !strcmp(name + ln - 18, "completionHandler:")) {
            __unsafe_unretained id blk = nil; [inv getArgument:&blk atIndex:(NSInteger)na - 1];
            if (blk) { void (^h)(id, NSError *) = [blk copy]; dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ h(g_null, nil); }); }
        }
    }
    // An error out-parameter stays as the caller set it (nil): nothing failed
    const char *rt = m.methodReturnType;
    while (*rt == 'r' || *rt == 'n' || *rt == 'N' || *rt == 'o' || *rt == 'O' || *rt == 'R' || *rt == 'V') rt++;   // type qualifiers
    if (*rt == 'v') return;
    if (*rt == '@') {
        __unsafe_unretained id r = null_returns_nil(s) ? nil : self;
        // new / copy / alloc / init families hand the caller an owned object, which ARC releases: balance it, the stand-in lives for the whole run
        if (r && (!strncmp(name, "new", 3) || !strncmp(name, "copy", 4) || !strncmp(name, "mutableCopy", 11) || !strncmp(name, "alloc", 5) || !strncmp(name, "init", 4))) CFRetain((__bridge CFTypeRef)r);
        [inv setReturnValue:&r];
        return;
    }
    NSUInteger len = m.methodReturnLength; if (!len) return;
    void *z = calloc(1, len); [inv setReturnValue:z]; free(z);
}
@end

id dm_null_obj(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_sigs = [NSMutableDictionary dictionary];
        g_nq = dispatch_queue_create("dm.nvn.null-gpu", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
        g_null = [DmNull new];
        NLOG("[RED] the null GPU stands in for Metal 4 (DM_NVN_NULL): the game runs and its draws are counted, nothing is drawn or presented");
    });
    return g_null;
}
MTL4CommitOptions *dm_null_commit_options(void) { return (MTL4CommitOptions *)(id)[DmNullCommitOptions new]; }
void dm_null_counts(unsigned long long *commits, unsigned long long *msgs) { *commits = atomic_load(&g_null_commits); *msgs = atomic_load(&g_null_msgs); }
