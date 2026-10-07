# PS2 hook core

`hooks.h` is a standalone C99 header; `hooks.hpp` adds a move-only C++17 handle
and a named register view from `registers.hpp`.
`hooks_guest.h` supplies the bounded guest backend. WFP's PS2 plugins use the
C++ frontends; `injector.h` remains the C API. Its `MakeTrampoline` uses this
core while retaining the existing callback signatures.

`patches.h` and `patches.hpp` provide C and C++ instruction-patch handles.
They save at most 16 original words inside the handle and allocate no code or heap
storage. `pcsx2_patch_create_call` replaces a JAL call site and preserves its delay
slot; `pcsx2_patch_original_target` returns the original callee. It rejects a call
inside a delay slot, control flow in the delay slot, and incompatible jump regions.
`pcsx2_patch_create_constant` changes adjacent LUI + ORI/ADDIU pairs together,
including the signed-low carry required by ADDIU. Neither helper relocates a call.
Creation prepares a disabled patch; enable, disable, destroy and C++ `reset()`
check expected instructions just like hook handles.

## Memory ownership

Every allocation belongs to the plugin that installs the hook. The backend must
allocate within that plugin's injected reservation and return failure at its
limit. It must never borrow game memory, another plugin's reservation, or an
unbounded allocator. Code allocation happens during creation, never during a mid
callback. Destruction returns code storage only after successfully restoring the
patch.

The PS2 SDK module runtime reserves a private heap before entry or constructors
run. Its `malloc`, `calloc`, `realloc` and reentrant newlib entry points are bounded
by `heap_begin..heap_end`. The default heap is 1 MiB; select a different budget in
the module's JSON `defines`, for example `"MODULE_HEAP_SIZE=262144"`. Stack, module
code and data are separate parts of the same injection reservation.

The legacy `AllocMemBlock` now obtains reusable arenas from this private heap.
Arena headers, block headers, alignment padding and other runtime allocations
consume the same finite budget. Defining `MEM_CUSTOM_TOTAL_SIZE` instead keeps
the legacy allocator in an embedded fixed pool. Neither mode grows past its
reservation. Exhaustion returns NULL.

Known arrays can be sized from their maximum element count at compile time.
Trampoline size is calculated when a hook is created. Arbitrary configuration,
recursion depth and runtime allocation patterns still need explicit upper limits;
the compiler cannot infer their complete peak memory use. Budget for metadata,
fragmentation, callback stack use and a measured margin as well as payload bytes.

## Backend and lifetime

Supply read, write, allocate, release and flush functions. A code buffer carries
its guest address separately from the pointer used to emit instructions. Both
must be 16-byte aligned; capacity is in bytes. A failed write must leave its whole
requested range untouched. A failed allocation must retain no storage.

Flush must publish generated instructions and invalidate affected emulator
translations. A CPU data-cache operation alone does not satisfy this contract.
Do not supply a no-op flush for live patches.

`pcsx2_injector_backend` binds the plugin's allocator and negotiated host cache
and code-write services. Standalone consumers can use `pcsx2_hook_guest_backend` with the 32-byte
module context and their bounded allocation functions. The service invalidates
translated instructions on the CPU thread. Service ABI 1 needs both capability
bits (context word 7 has ABI 1 and capability bits 1 and 2). Cache invalidation is at `0x02000020`;
instruction writes use `0x02000040` with destination, source and byte count.
The host copies the entire range and invalidates it before guest execution resumes.
It rejects misaligned instruction ranges, overflow and writes into its service page.
Older hosts return
`PCSX2_HOOK_BACKEND_UNAVAILABLE` before patching; update the emulator/injector.

Legacy instruction helpers now use this backend. `MakeJMPwNOP` and inline
installation publish jump/NOP pairs together; a stub is complete and flushed
before its entry jump appears. Allocation or publication failure frees a new
stub without installing it. A branch at the end of a legacy inline range is
relocated with its delay slot, preserving branch-likely annulment. Constant
stubs execute the suffix exactly once; adjacent constant pairs publish together
without allocating a trampoline. Generic `WriteMemory*`
and `MemoryFill` remain data accessors without instruction-cache work. Use
`WriteInstr` for code, and inspect `pcsx2_injector_last_patch_status()` after legacy
void helpers. Bulk NOP helpers publish chunks of at most 16 words and need the
same quiescent installation context as other patch operations.
`pcsx2_injector_patch_failures()` counts failed legacy patch operations cumulatively.
The legacy constant search stops at control flow, another definition of the
register, or a coprocessor transfer consuming it. It cannot pair a LUI with an
ORI from a later constant or alternate path. If a constant stub moves an
adjacent LUI, a module-owned record keeps its original address editable by later
constant helpers. That suffix reserves a LUI/ORI pair; updates publish both words
together. Records and permanent stubs live until module reset/unload, and consume
the same bounded allocator. Lookup and allocation occur only during patching.
An unexpected non-LUI address reports `UNSUPPORTED_INSTRUCTION` rather than
silently skipping the patch.

Initialize the handle to zero. `pcsx2_hook_create_inline` and
`pcsx2_hook_create_mid` prepare disabled hooks. Call `pcsx2_hook_enable` to install;
`pcsx2_hook_disable` restores the original instructions. Enable, disable and
destroy compare memory against the expected patch to detect conflicts. On a
restore failure the handle retains its code allocation, avoiding a live jump
into freed storage. The C++ destructor follows the same rule; use `reset()` and
check its status when recovery is needed.

Creation, installation, destruction and allocator operations must be serialized
on a quiescent guest execution context. They are not thread-safe. Do not destroy
a trampoline while a callback or an original-function call is executing it.

## Instruction and callback contract

Hooks overwrite at least two instructions. Capture includes the delay slot of a
final branch. Relocation supports ordinary instructions, conditional branches
(including likely branches), J, JR, JAL, JALR and the linking REGIMM branches
(BLTZAL/BGEZAL and their likely forms). Internal targets are remapped; far
conditional branches preserve delay-slot execution and annulment. A call pair
that ends the window loads the original return address (the window end) and
jumps, so the callee returns directly to game code with the exact original link
value. A call earlier in the window links into the trampoline, which runs the
rest of the window and jumps back; the callee then observes a trampoline return
address, and the hook's code must not be destroyed while that callee can still
return. JALR with rd == rs and linking branches testing ra are rejected, as are
syscalls, breaks, traps and privileged operations. J/JAL targets must be in the
same 256 MiB jump region as the trampoline. Failures occur before installing the hook.

Mid callbacks run before relocated instructions, with arguments
`pcsx2_hook_context*` and `void*`. A nonzero module GP selects that GP; zero retains
the incoming game GP, useful for `-G0` callbacks that call game functions. Register
edits are applied on return. All 128 bits of GPRs, HI and LO, and SA are saved.
`PCSX2_HOOK_SAVE_FPU` additionally saves FPRs and FCR31. Fields for state excluded
by flags are unspecified. The generated frame consumes 752 stack bytes, in
addition to the callback's own frame and callees.

`PCSX2_HOOK_RAW_CALLBACK` retains the incoming argument registers instead of
passing a context and user pointer. It supports register arguments only; stack
arguments are not copied into the callback frame. The legacy adapter uses this
mode and retains the game GP. Its return value is the callable original
trampoline, without reentering the callback. Code is retained for module lifetime.

`PCSX2_HOOK_SAVE_ACC` additionally preserves EE floating-point ACC and its
overflow flag. `PCSX2_HOOK_SAVE_VU0` preserves VU0 vector/integer registers,
accumulator, Q/P and internal arithmetic pipelines through an optional host
service at `0x02000060` (capability bit 4). It finishes an outstanding VU0
microprogram before capture. It preserves live VU1 status and host pointers;
it does not snapshot VU1, VIF, or VU micro/data memory. Callbacks must leave
those resources intact. The opaque image returned by `pcsx2_hook_vu0()` is
read-only. VU0 preservation adds 2048 bytes to the frame, for 2800 bytes total,
and two host service calls; enable it only where the callback needs it.
ACC preservation alone keeps the 752-byte frame. Creation fails before
patching when these flags require a service the host does not provide.
Without these flags, callbacks and their callees must leave ACC/VU0 intact.

Inline replacements use the game function's calling convention;
compile without GP-relative small data or PIC (`-G0 -fno-pic -mno-abicalls`), or
provide an appropriate entry thunk.

## C++ plugin API

Failure reporting: in a checked startup (`Initialize`/`InitializeCheckedRuntime`) any failure rejects the whole session at `FlushCaches`, which then calls the `on_error` handler exactly once. `injector::FirstFailure()` returns the first failing game site and status since the last initialization and `injector::StatusName(status)` names the reason, so a plugin can show e.g. "disabled: window holds a non-relocatable instruction at 004A1CC0". In `DEBUG`, `_DEBUG` or `WFP_PS2_DEBUG` builds the first failure is also written to `logger` when the module links `log.c` (weak reference; otherwise skipped).

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
if (injector::Initialize(backend) != PCSX2_HOOK_OK)
    return -1;
Render.fun = injector::MakeCALL(render_site, &RenderHook).get();
injector::MakeCALL(second_render_site, &RenderHook);
injector::WriteMemory<uint16_t>(data_address, 1);
static auto argument_hook = safetymips::create_mid(argument_site,
    [](SafetyMipsContext& regs) { regs.a1 = 1; });
return injector::FlushCaches() == PCSX2_HOOK_OK ? 0 : -1;
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
checked instruction core. Preserve patching order when migrating existing
stubs; rewriting their intent into register callbacks is a separate change.

## Standalone C and lower-level C++

`hooks.h`, `patches.h` and `memory.h` are standalone C99 headers. `hooks.hpp` and
`patches.hpp` provide explicit move-only C++ handles without a global frontend.
Their create operations prepare disabled handles, followed by checked enable,
disable and destroy. These remain useful for custom runtimes and explicit lifetime
control. The legacy helpers stay available for existing plugins during migration.
All layers use the same checked core and bounded backend.

PS2 also aliases `a4`..`a7` to `t0`..`t3`. Full EE GPR lanes remain
accessible through `raw().gpr[index].word[0..3]`. The default mid-hook
preservation includes FPU state. VCS ImVehLM uses scalar `MakeCALL`
and `hook_back` with the existing private heap and unchanged texture storage.

## Validation

Run `python tests/pcsx2-hooks/run.py` from WFP on Windows with Visual Studio.
It compiles both C/C++ headers, executes emitted instructions in a small MIPS
test CPU, and stress-tests both fixed pools and the actual 32-bit SDK heap.
These checks do not replace PCSX2 JIT or in-game validation.
The guest probe additionally passed enable/disable/re-enable, original calls,
context edits and raw callbacks in the fork and source-matched stock v2.9.94,
after executing the target once to compile it in the EE JIT. Call-site patches
preserved argument changes in delay slots and the original return address.
Signed-low constant patches, restoration and legacy inline publication also
passed after JIT compilation. VCS ImVehLM, Project2DFX, WidescreenFix and rain
plugins were checked at startup on both hosts. The later strict constant
decoder identified one previously skipped non-LUI VCS WidescreenFix target
(`0x00118E34`); it remains untouched and is now reported as unsupported.
Address resolution and successful installation alone do not prove every feature
works: the later VCS HUD fixes also have targeted executable regression tests.
Additional guest probes verified taken/untaken branches in legacy inline tails
and ACC/VU0 restoration after a callback deliberately clobbered those registers.
The probe also exhausts and reuses its bounded pool and runs 256 complete
inline/mid creation, invocation and removal cycles on each host.

Pattern scanning parses signatures once into bounded stack storage, supports
`?`/`??` wildcards and overlapping matches, and rejects malformed or oversized
signatures without allocation. It is intended for initialization, not callbacks.
