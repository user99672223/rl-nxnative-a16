# Rocket League (01005EE0036EC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID (CUTS, STUBS as described there, "Registered patches").

# 1.0.0 (v0) main: Unreal Engine 3's RHIKickCommandBuffer (main+0xa95968: adrp x0 / ldr x0 / mov w1, #1 / mov w2, wzr / b main+0xab5ed4, the RHI's "end this command list,
# submit it and flush the queue, do not wait"). The scene renderer calls it between its phases and after every post-process step (main+0x7e5040, +0x7ef16c, +0x7ef1a8, +0x7f0398,
# +0x7f093c, +0x7f10e0, +0x837b5c, +0xab5fac, +0xa2c408), so the GPU starts on the frame early. Each kick ends the command list, and with it the open render pass: a menu frame's
# last 11 draws go to the same 1280x720 target as 11 passes of one draw, each loading and storing the whole target (pass dump of run 43169272803), which on a tile-based GPU is
# the frame's largest avoidable cost. The kick is no synchronization (it waits for nothing; the frame's end, a full ring of command memory and every read of GPU results submit
# through other paths), so it can go: the stub returns at once while the flag is set, and the frame's draws reach the replay in fewer, longer command lists whose passes stay open.
# The flag is the system setting SpeedTreeLeaves (main+0x2e2adc0), which no code of this build reads (it is only in the settings table); the host holds it through the table
# (hle_ue3set.c, DM_UE3_KICK): 1 = no kicks, 0 = the game's own.
from nso_asm import ADRP, B

_KICK, _KICK_FIRST, _FLAG = 0xa95968, 'c01301d0', 0x2e2adc0


def kick_gate(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(_KICK, _KICK_FIRST)
    start = here()
    emit(ADRP(16, here(), _FLAG))                                   # adrp x16, page of the flag   (x16 is dead at a call)
    emit(0xB9400000 | ((_FLAG & 0xfff) // 4) << 10 | 16 << 5 | 16)  # ldr w16, [x16, #0xdc0]
    emit(0x34000000 | 2 << 5 | 16)                                  # cbz w16, +8: the game's own kick
    emit(0xd65f03c0)                                                # ret
    emit(ADRP(0, here(), 0x2d0f000))                                # the original first instruction, for this place
    emit(B(here(), _KICK + 4))
    ctx.put(_KICK, [B(_KICK, start)], "RHIKickCommandBuffer returns at once while the host's flag is set (the frame's passes stay open across the renderer's kicks)")


# 1.0.0 (v0) main: the RHI's memory pool of render targets ("Targets", pool 2), made at start with a fixed size (main+0xab4698: mov w1, #0x11e10000, 286 MB, sized for the
# game's own 1280x720). With the engine's ResX / ResY held at 2560x1440 (the player's Render resolution, hle_ue3set.c) the scene's targets are four times the size, the pool
# runs out and the texture's storage comes back null (run 43172097281: a fault at main+0xa9ca88 reading it). The size follows ResX, which the host holds before the pools are
# made (nvnDeviceInitialize comes first in the same function): above 1280 across 656 MB (2.25 x the pixels of 720p), above 1920 across 1,152 MB (4 x). The pool is address
# space of the game's heap until targets are made in it.
_POOL, _POOL_FIRST, _RESX = 0xab4698, '213ca252', 0x2e2b1d0


def target_pool(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(_POOL, _POOL_FIRST)
    start = here()
    movz = lambda imm16: 0x52A00000 | imm16 << 5 | 1                 # movz w1, #imm16, lsl #16
    cmp16 = lambda imm: 0x7100001F | imm << 10 | 16 << 5              # cmp w16, #imm
    ble = lambda words: 0x54000000 | words << 5 | 0xd                 # b.le +words
    emit(ADRP(16, here(), _RESX))                                    # adrp x16, page of ResX   (x16 is dead here: arguments are being set for a call)
    emit(0xB9400000 | ((_RESX & 0xfff) // 4) << 10 | 16 << 5 | 16)   # ldr w16, [x16, #0x1d0]
    emit(movz(0x11e1))                                               # the game's own size
    emit(cmp16(1280)); emit(ble(5))
    emit(movz(0x2900))                                               # 656 MB
    emit(cmp16(1920)); emit(ble(2))
    emit(movz(0x4800))                                               # 1,152 MB
    emit(B(here(), _POOL + 4))
    ctx.put(_POOL, [B(_POOL, start)], "the render-target pool's size follows the held ResX (286 MB at 720p, 656 MB above it, 1,152 MB above 1080p)")


# 1.0.0 (v0) main: the scene's sizes. The scene render targets' object (FSceneRenderTargets, at the pointer main+0x2d0cdd0) keeps a table of the sizes its dynamic resolution
# steps through (+0xab8, +0xaac entries, the current step at +0xaa8), filled once when the window is made (main+0x809fd8): 1280x720, 1152x648, 1024x576, 896x504, 768x432,
# constants. Every scene view takes its size from the current step (main+0x80a17c), so with ResX / ResY held at 2560x1440 the targets and the window were 2560x1440 and the scene
# was still drawn 1280x720 into their top-left corner, the rest left as it was (an iPhone 15, Render resolution 1440p: half the width and height; 1080p: two thirds; pass dump
# of run 43483309451: viewports 0,0,1280,720 on 2560x1440 targets). The stub fills the table from the held ResX / ResY instead, in the game's own steps of a tenth
# (100 %, 90 %, 80 % ...), rounded down to 8 pixels; at the game's own 1280x720 the table is the one it had.
_RTAB, _RTAB_FIRST, _RTAB_END = 0x80a00c, '08a08052', 0x80a06c


def res_table(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(_RTAB, _RTAB_FIRST)
    start = here()
    ldrw = lambda t, n, off: 0xB9400000 | (off // 4) << 10 | n << 5 | t
    strw = lambda t, n, off: 0xB9000000 | (off // 4) << 10 | n << 5 | t
    movz = lambda t, imm: 0x52800000 | imm << 5 | t
    emit(ADRP(16, here(), _RESX))                       # x16 is free here (the function's own registers are x19, x29, x30)
    emit(ldrw(9, 16, _RESX & 0xfff))                    # w9 = ResX
    emit(ldrw(10, 16, (_RESX + 4) & 0xfff))             # w10 = ResY
    emit(0x35000000 | 3 << 5 | 9)                       # cbnz w9, +3: a size was set
    emit(movz(9, 1280)); emit(movz(10, 720))
    emit(ldrw(11, 19, 0xaac))                           # w11 = entries
    emit(movz(8, 0)); emit(movz(17, 10))
    loop = here()
    emit(0x6B00001F | 11 << 16 | 8 << 5)                # cmp w8, w11
    emit(0x54000000 | 16 << 5 | 2)                      # b.hs end (16 words on: the 15 of the loop's body and its branch back)
    emit(0x4B000000 | 8 << 16 | 17 << 5 | 12)           # sub w12, w17, w8        tenths
    for d, src in ((13, 9), (14, 10)):
        emit(0x1B000000 | 12 << 16 | 31 << 10 | src << 5 | d)   # mul wd, wsrc, w12
        emit(0x1AC00800 | 17 << 16 | d << 5 | d)                 # udiv wd, wd, w17
        emit(0x53000000 | 3 << 16 | 31 << 10 | d << 5 | d)       # lsr wd, wd, #3
        emit(0x53000000 | 29 << 16 | 28 << 10 | d << 5 | d)      # lsl wd, wd, #3
    emit(0xF9400000 | (0xab8 // 8) << 10 | 19 << 5 | 15)         # ldr x15, [x19, #0xab8]
    emit(0x8B204C00 | 8 << 16 | 15 << 5 | 15)                    # add x15, x15, w8, uxtw #3
    emit(strw(13, 15, 0)); emit(strw(14, 15, 4))
    emit(0x11000400 | 8 << 5 | 8)                                # add w8, w8, #1
    emit(B(here(), loop))
    assert here() == loop + 4 * 17, 'res_table: the loop body changed length; fix the b.hs offset'
    emit(B(here(), _RTAB_END))
    ctx.put(_RTAB, [B(_RTAB, start)], "the scene's dynamic-resolution sizes follow the held ResX / ResY (1280x720 and its tenths as shipped)")


def stubs(ctx): kick_gate(ctx); target_pool(ctx); res_table(ctx)


STUBS = {'becfed979bb4204fbd0945cd450fb296': stubs}   # 1.0.0 (v0) main

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': "the scene's dynamic-resolution sizes follow the held ResX / ResY", 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "a table of constants in the port's scene render targets"},
    {'what': "the render-target pool's size follows the held ResX", 'now': 'build', 'goal': 'game', 'purpose': 'memory',
     'why': "the port's own pool sizes, constants in its RHI start"},
    {'what': "RHIKickCommandBuffer returns at once while the host's flag is set", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "Unreal Engine 3's kick in Panic Button's NVN RHI; a function-matched rule could carry it to the game's updates"},
]
