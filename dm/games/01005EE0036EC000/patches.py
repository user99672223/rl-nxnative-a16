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


STUBS = {'becfed979bb4204fbd0945cd450fb296': kick_gate}   # 1.0.0 (v0) main

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': "RHIKickCommandBuffer returns at once while the host's flag is set", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "Unreal Engine 3's kick in Panic Button's NVN RHI; a function-matched rule could carry it to the game's updates"},
]
