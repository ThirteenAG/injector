# PSP hook core

`hooks.h` is a standalone C99 header; `hooks.hpp` adds a move-only C++17 handle
and a named register view from `registers.hpp`.
`hooks_guest.h` supplies the bounded guest backend. WFP's PSP plugins use the
C++ frontends; `injector.h` remains the C API. Its `MakeTrampoline` uses this
core while retaining the existing callback signatures.

`patches.h` and `patches.hpp` add C and C++ instruction-patch handles without
heap or code allocation. They retain up to 16 original words in the handle.
`psp_patch_create_call` replaces a JAL call site while preserving its delay slot;
`psp_patch_original_target` returns the original callee. Calls inside delay slots,
control flow in delay slots and incompatible jump regions are rejected.
`psp_patch_create_constant` changes adjacent LUI + ORI/ADDIU pairs together and
handles ADDIU's signed-low carry. Creation prepares a disabled patch; enable,
disable, destroy and C++ `reset()` check expected instructions before changing them.

## PRX logging configurations

The PSP SDK builder defaults to Release. Use `-Configuration Debug` for debug
symbols and `DEBUG`/`_DEBUG` defines; Release defines `NDEBUG`. Visual Studio
passes its selected configuration to the builder. Makefiles accept
`CONFIGURATION=Debug`. Both configurations retain the existing optimized guest
code and use the same output path, so a Release build replaces a prior Debug PRX.

`logger.WriteF` and `logger.Write` remain available in every configuration.
Guard individual diagnostic call sites with `#if (defined(DEBUG) || defined(_DEBUG))
&& !defined(NDEBUG)` to omit those calls and their argument evaluation in Release.
WFP's new PSP hook-installation diagnostics and the C++ starter use this guard;
existing plugins retain their own logging policy.

## Fixed plugin budget

The legacy allocator uses storage embedded in the plugin's PRX. Its default
budget remains 100,000 bytes. Override it in the module JSON `defines`, for
example `"MEM_CUSTOM_TOTAL_SIZE=131072"`. Allocations, 16-byte alignment padding
and block headers all consume that pool. Exhaustion returns NULL; no kernel or
game allocation is used as a fallback. Freed blocks are reused and adjacent free
blocks coalesced. Initialization does not discard live allocations.

A backend for the new API must follow the same ownership rule: allocate only
from storage reserved for this plugin, with a hard ceiling. Create code once per
hook; mid callbacks perform no allocations in the generated wrapper. Choose
budgets from maximum configured array sizes and measured peak use, allowing for
metadata and fragmentation. Runtime-dependent demand cannot generally be
computed completely at compilation time.

WFP's rewritten LCS Project2DFX embeds the maximum 1,024 corona records directly
in its module image and reserves 32 KiB separately for hooks. The two reservations
cannot consume each other's storage.

## Calls between game and SDK code

Native-game compilers can preserve even f20..f30 while PSP SDK EABI preserves
f20..f27. Add `PSP_GAME_ABI_COMPAT` to the JSON manifest when using native-game
callbacks. The SDK builder makes f21/f23/f25/f27 caller-saved and reserves f28/f30,
so direct typed calls into either convention do not lose compiler temporaries.
The C++ injector frontend then adapts typed call/jump/inline callbacks behind
the usual API, preserving f20..f31, GP and FCSR, and forwarding overflow stack
arguments. Its bounded per-module cache creates each adapter once; the running
callback performs no lookup or allocation. Raw integer-address stubs keep their
explicit ABI contract. The standalone C emitter is in `game_abi.h`.

An integer-only inline callback needing the native return address can set
`Options::preserve_game_callees=false`. It must not call floating-point SDK
routines or modify native callee registers. An adapted callback's return address
belongs to its adapter; do not use it to identify the game's caller.

## Backend and lifetime

Supply read, write, allocate, release and flush functions. Code buffers carry a
guest address, an emission pointer, byte capacity and an allocation token. The
address and pointer must be 16-byte aligned. Failed writes must leave the whole
requested range untouched; failed allocations must retain no storage. Flush
must publish generated code and invalidate emulator translations as well as
relevant guest caches. A no-op flush does not satisfy this contract.

`psp_injector_backend` binds the embedded allocator. Standalone consumers can use
`psp_hook_guest_backend` with their storage range and bounded allocation
functions. Writes suspend interrupts and publish data/instruction caches before
resuming, so another guest thread cannot see half of a patch. Allocator and hook
lifetimes still require caller serialization.

The guest backend accepts cached physical RAM, including the kernel-module
partition used by CLEO. It rejects uncached/kernel aliases, scratchpad and MMIO.
`PSP_HOOK_RAM_END` defaults to `0x0A000000` (32 MiB). Set it in every translation
unit to match the emulator's mapped RAM: WFP, CLEO and the template use
`"PSP_HOOK_RAM_END=0x0DD00000"` with PPSSPP `plugin.ini` `memory = 93`.
This access bound does not enlarge any allocator: generated code must still fit
the separately supplied, module-owned storage range.

Instruction reads invalidate the range before decoding or comparing it. PPSSPP
can replace a compiled block's first instruction with a JIT entry marker; cache
invalidation restores the original opcode. This handles hooks on functions that
have already executed without storing or relocating an emulator marker. The
extra cache work happens during preparation and patch changes, never callbacks.

Legacy instruction helpers now use that backend too. Inline stubs are completed
and flushed before publication; jump/NOP pairs install together. Failed publication
frees a new stub without installing it. A branch at the end of a legacy inline
range is relocated with its delay slot; branch-likely annulment is retained.
Constant stubs execute the original suffix exactly once, and adjacent constant
pairs are patched together without allocating a stub. Generic `WriteMemory*` and
`MemoryFill` remain data accessors; use
`WriteInstr` for code. `psp_injector_last_patch_status()` exposes failure from legacy
void helpers. Bulk NOP helpers publish chunks of at most 16 words and require a
quiescent installation context for the whole range.
`psp_injector_patch_failures()` counts failed legacy patch operations cumulatively.
Constant searches stop at a new definition, a coprocessor transfer consuming the
register, or control flow. They cannot match an ORI belonging to a later constant
or alternate path. When a stub moves an adjacent LUI, a module-owned record keeps
its logical address editable by subsequent constant helpers. The moved suffix
reserves a LUI/ORI pair, allowing complete updates after JIT compilation. Records
and permanent stubs consume the bounded pool and live until module unload.
There is no lookup or allocation in game callbacks. An unexpected non-LUI
address reports `UNSUPPORTED_INSTRUCTION` instead of silently skipping it.

Zero-initialize handles. Creation prepares a disabled hook; enable installs it.
Disable restores original instructions. Each change compares the expected
instructions and reports conflicts. Destroy frees code only after successful
restoration. A failed restore deliberately retains storage rather than leaving
an installed jump into freed memory. Check explicit C++ `reset()` results when
recovery matters.

Serialize creation, installation, destruction and allocator operations while
guest execution is quiescent. Do not release a trampoline while any thread is
executing it. These operations are not thread-safe.

## Callback and relocation contract

Mid callbacks run before relocated instructions, with arguments
`psp_hook_context*` and `void*`; register edits are applied on return. The wrapper
sets a nonzero supplied module GP; zero retains the incoming game GP. It saves
GPRs, HI and LO. Optional flags preserve
FPRs/FCR31 and VFPU registers/control state. Excluded fields are unspecified.
The wrapper consumes 880 stack bytes in addition to callback and callee frames.
VFPU instructions match the actual Allegrex assembler. The prefix registers
(VPFXS/VPFXT/VPFXD) are saved into the context but never written: a hook site never
splits a prefix from its consumer, and writing them before non-VFPU code leaves an
"uneaten" prefix that makes PPSSPP disable JIT optimisations for the session.

`PSP_HOOK_RAW_CALLBACK` retains incoming register arguments instead of passing a
context and user pointer. Stack arguments are not copied into the callback frame.
The legacy adapter uses this mode, retains game GP, and saves FPU/VFPU state. It
returns the callable original trampoline and retains code for module lifetime.
Select only the state needed by a new callback and its callees: VFPU preservation
saves and restores 128 vector registers per invocation. No generated callback allocates.

Inline replacements use the game's calling convention. Avoid GP-relative small
data/PIC or provide an entry thunk. Capture starts outside a delay slot, covers
at least two instructions and includes a final branch's delay slot. The
relocator supports conditional and likely branches (including VFPU branches),
J, JR, JAL, JALR and the linking REGIMM branches, including internal targets. A
call pair that ends the window loads the original return address (the window
end) and jumps, so the callee returns directly to game code with the exact
original link value. A call earlier in the window links into the trampoline,
which runs the rest of the window; the callee then observes a trampoline return
address, and the hook's code must stay alive while that callee can return.
JALR with rd == rs, linking branches testing ra, syscalls, breaks, traps and
privileged operations are rejected. J/JAL targets must fit the same 256 MiB jump
region. A preparation failure leaves the game patch untouched.

## C++ plugin API

Failure reporting: in a checked startup (`Initialize`/`InitializeCheckedRuntime`) any failure rejects the whole session at `FlushCaches`, which then calls the `on_error` handler exactly once. `injector::FirstFailure()` returns the first failing game site and status since the last initialization and `injector::StatusName(status)` names the reason, so a plugin can show e.g. "disabled: window holds a non-relocatable instruction at 004A1CC0". In Debug (`NDEBUG` undefined) builds the first failure is also written to `logger` when the module links `log.c` (weak reference; otherwise skipped).

Include `injector.hpp` or `safetymips.hpp` for the Windows-style frontend. Both
expose the same API; include only one platform's frontend in a translation unit.
Set up the module-owned backend once, then write ordinary scalar patches:

```cpp
#include "injector.hpp"

injector::hook_back<int (*)(void*)> Render;

int RenderHook(void* entity)
{
    return Render.fun(entity);
}

// During module initialization, after creating the guest backend:
if (injector::Initialize(backend) != PSP_HOOK_OK)
    return -1;
Render.fun = injector::MakeCALL(render_site, &RenderHook).get();
injector::MakeCALL(second_render_site, &RenderHook);
injector::WriteMemory<uint16_t>(data_address, 1);
static auto argument_hook = safetymips::create_mid(argument_site,
    [](SafetyMipsContext& regs) { regs.a1 = 1; });
return injector::FlushCaches() == PSP_HOOK_OK ? 0 : -1;
```

`Initialize` starts a deferred installation session. The scalar calls prepare
patches without changing game memory. `FlushCaches` checks the complete session,
publishes its patches and invalidates translations. Failed installation restores
already published patches. Failure to restore a live patch stops execution rather
than unloading code still referenced by the game. An optional error handler passed
to `Initialize` receives failures; `LastError()` and the final status remain available.

When `.get()` requests an original callee, the session automatically checks other
call sites using that same replacement. No explicit site count, callee comparison,
commit object or rollback block is needed. Different original callees may share a
replacement when that replacement does not request an original through `.get()`.
Queued data reads see previous queued data writes. Overlapping owning hooks and
incompatible edits are rejected. Temporary journal records use the same bounded
plugin allocator and are freed at completion. There is no journal or allocation
in a game callback. Permanent scalar patches remain installed after completion.

Outside a session, patches publish immediately. `SetBackend` selects this mode
directly. Explicit `patch_batch<N>` and `InstallCalls` remain optional for callers
that need fixed preparation storage or explicit groups.

| Windows-style operation | MIPS frontend behavior |
| --- | --- |
| `MakeCALL`, `hook_back` | Replace a JAL while keeping its delay slot; return its original callee. |
| `MakeJMP`, `MakeRET` | Write J/JR plus a NOP delay slot; overwrite two instructions. |
| `MakeNOP`, `MakeRangedNOP` | Lengths are bytes and must be multiples of four. |
| `GetBranchDestination`, `SetBranchDestination` | Decode/update direct jumps and supported conditional branches; check alignment and range. |
| `WriteConstant` | Update an adjacent LUI + ORI/ADDIU pair, including signed-low carry. |
| `ReadMemory<T>`, `WriteMemory<T>`, raw/object helpers, `MemoryFill` | Preserve neighboring bytes for unaligned data and publish through the backend. |
| `memory_pointer` | Address arithmetic and typed `.get()` / `.get<T>()` conversion. |
| `scoped_call`, `scoped_jmp`, `scoped_write<N>` | Restore owned patches on destruction; explicit `reset()` reports conflicts. |
| `cstd<Return(Args...)>::call` | Call a guest function using the platform's normal ABI. |
| `safetymips::create_inline` | Own a function detour with a callable original trampoline. |
| `safetymips::create_mid` | Own a named-register callback; replay displaced instructions by default. |
| `MakeInline<Functor>` | Permanent named-register callback replacing the displaced instructions. |
| `MakeInlineOnce<Functor>` | Restore the original site before the first callback; later calls have no hook overhead. |

An inline detour's `.call<Return>(args...)` invokes its original trampoline;
`.original<FunctionPointer>()` provides a typed pointer. Owned SafetyMips handles
are move-only and must outlive their installed hooks. Keep them in module/static
storage, or explicitly restore them before leaving a scope. Pending handles may
be moved; destroying one before `FlushCaches` cancels it. `.error()` reports its
preparation/publication status, and `.enabled()` distinguishes prepared from live.

`create_mid` accepts a noncapturing lambda or a compile-time named callback:
`create_mid<&Callback>(address, user_pointer, options)`. `Options` selects the
instruction count, preserved state, module GP and whether to execute the original
instructions. `execute_original = false` skips them on the callback path while
retaining them in the callable original trampoline. `MakeInline` uses this
replacement behavior to match the PC helper. Its stateless functor receives
`injector::reg_pack&`, an alias of `SafetyMipsContext`.

`MakeInlineOnce` keeps its code and small state record until module reset, because
its first callback is still returning through that code. It allocates only during
setup. The first invocation performs checked restoration/cache invalidation before
calling the functor; recursive calls therefore use the restored game code.

Named GPRs borrow the low 32-bit saved words. Assigning them preserves other saved
lanes; `zero` is read-only and `fp` aliases `s8`. `raw()` exposes the underlying C
context. Float registers require FPU preservation; `.bits()` accesses their exact
word. Float conversion uses compiler bit-casting or a memcpy fallback, without
pointer aliasing. Copying float fields preserves NaNs. Contexts and field references
must not escape the callback.

Noncapturing lambdas compile directly into the callback thunk. There is no
`std::function`, runtime callback dispatcher or callback allocation. Actual SDK
assembly for `regs.a1 = 1` is a constant load, return and direct store in the return
delay slot, without a view-construction frame. State-save costs still apply:
preserve everything the callback and its callees touch.

The guest pointer width is 32 bits. Windows calling-convention annotations, x86
relative-displacement helpers and page protection functions have no corresponding
MIPS operation. Use the normal guest ABI, direct-branch helpers and backend cache
publication. Do not include the legacy C `injector.h` alongside `injector.hpp`;
its global `injector` object conflicts with the frontend namespace.

## Explicit instruction stubs

Include `runtime.hpp` and call `injector::InitializeRuntime()` when retaining
existing explicit MIPS stubs or nonadjacent/overlapping constant edits. Link
`injector.c`, `mips.c` and `memalloc.c` as C. Instruction builders are in
`mips_asm`; `injector::MakeInline(address, words...)`, `MakeInlineWithNOP`,
`MakeInlineLUIORI` and `MakeInlineLI` use the same checked C emitter. This keeps
the small, individually audited instruction sequences rather than replacing
them with full register-save callbacks. `HighWord`/`LowWord` extract float bits
without pointer aliasing. `GetGP` is inlined in SDK builds.

This initialization mode publishes instructions immediately, in source order.
It preserves legacy constant-suffix association and each plugin's final cache
reset. It does not provide a deferred installation journal or automatic
shared-callee checks for its immediate call replacements. Use `Initialize`
with the scalar/owning frontend for those checks. Data operations in runtime
mode retain the original direct access: aligned scalar reads/writes use typed
loads/stores, and unaligned/raw objects use memcpy. They do not allocate or
invoke instruction-cache services; use `WriteInstr` for executable code.

The runtime mode and the deferred frontend share the bounded allocator and
checked instruction core. Preserve patching order and PSP startup delays when
migrating existing stubs; rewriting their intent into register callbacks is a
separate change.

## Standalone C and lower-level C++

`hooks.h`, `patches.h` and `memory.h` are standalone C99 headers. `hooks.hpp` and
`patches.hpp` provide explicit move-only C++ handles without a global frontend.
Their create operations prepare disabled handles, followed by checked enable,
disable and destroy. These remain useful for custom runtimes and explicit lifetime
control. The legacy helpers stay available for existing plugins during migration.
All layers use the same checked core and bounded backend.

PSP defaults preserve FPU/VFPU state. VCS GamepadIcons and ImVehLM use
scalar `MakeCALL` and `hook_back`, with a 4 KiB embedded hook pool each.
Their original `module_start` paths and 120 ms waits remain intact; they
do not require a CRT heap or global constructors.

Run `python tests/pcsx2-hooks/run.py` from WFP for the current native allocator,
instruction and C/C++ API checks. PPSSPP JIT and in-game tests remain separate.
The PPSSPP guest probe passed call-site delay slots, signed-low constants,
restoration, legacy inline publication, mid/raw callbacks and FPU/VFPU/prefix
preservation after executing the targets to compile them in the JIT. A native
regression test also checks decoding and restoring PPSSPP-style entry markers.
The guest probe also exhausts and reuses its bounded pool and runs 256 complete
inline/mid creation, invocation and removal cycles in the emulator.
