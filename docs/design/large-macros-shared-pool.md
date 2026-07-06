# Design: relax the per-macro size limit — shared large-value pool across slots

**Status:** design only — implementation to be done by follow-up (Sonnet) sessions.
**Author:** design worked out 2026-07-06/07 (v3).
**Scope:** two parts. **Part A** is an *upstream* extension to
`zmk-feature-custom-settings` (a shared backing pool for large values); **Part B** is
the `zmk-feature-runtime-macro` migration that consumes it.
**Upstream dependency:** `zmk-feature-custom-settings` **PR #24**
(`codex/large-value-storage`, "Support setting values larger than 64 bytes
(per-setting max_size + end-to-end chunked transport)", closes issue #16) — **not yet
merged** at time of writing. Part A is designed to stack on that branch (or fold into
it, at the owner's discretion). Also assumes PR #15 (P3 arrays), already on `main`.
**Backward compatibility:** *not required* (explicitly waived by the repo owner).
Stored settings schema, RPC surface, and Kconfig may all change; saved macros on a
device may be discarded on first boot of the new firmware.

> **Design lineage:** v1 was a runtime-macro-local shared chunk pool over P3 arrays
> (written before PR #24 existed). v2 rebased onto PR #24 as N per-slot
> `ZMK_CUSTOM_SETTING_DEFINE_SIZED` settings — simple, but resident RAM is
> `COUNT × MAX_BYTES` even for empty slots. v3 (this document) restores the shared-
> budget property of v1 *without* the module-local machinery, by adding the pool
> **upstream** where PR #24 already put the large-value mechanism. §11 records the
> alternatives.

---

## 1. Problem

Each runtime macro's body is stored as **one** custom-settings value of type `BYTES`,
capped by `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` — pinned to **64 bytes** whenever
the protobuf RPC is enabled (which `ZMK_RUNTIME_MACRO_STUDIO_RPC` always selects).

Facts that shape the fix:

- **The RPC is not the bottleneck.** Runtime-macro's Studio RPC transfers macros *step
  by step* (`SetMacroStepCount`/`SetMacroStep` up, `repeated MacroStep` down). The
  limit is purely firmware storage.
- **Raising the global `VALUE_MAX_SIZE` is the wrong lever.** Pinned to 64 by the
  protobuf schema; sizes every setting's value union in every module.
- **PR #24 solves single-setting size** via per-setting `max_size` + a dedicated
  static `large_data` buffer per sized setting, accessed only through
  `zmk_custom_setting_read_into`/`zmk_custom_setting_write_bytes` and the chunked RPC.
- **What PR #24 does *not* provide: a shared budget.** N macro slots as
  `DEFINE_SIZED` settings cost `N × (max_size + 1)` resident bytes even when most
  slots are empty. The goal here — "let one macro be big without paying `N × max`" —
  wants the slots to draw from **one shared pool**.

## 2. Why a shared pool can live upstream (verified against PR #24's code)

Two properties of PR #24's implementation make a pooled backing store a natural
extension rather than a redesign:

1. **`large_data` is already a per-setting pointer** (not an embedded array), assigned
   at registration. Every access site funnels through
   `setting_uses_large_store()` → `setting->large_data` **under `settings_lock`**
   (`large_store_set_raw/_set_value`, `effective_value`'s scratch materialization,
   the `read_into`/`write_bytes` large paths, save/load staging).
2. **No caller retains a pointer into `large_data` across lock drops.** Even the
   multi-frame chunked-RPC read re-snapshots the whole value into the module-global
   `chunk_read_buffer` via `read_into` before each response; firmware callers get
   copies (`read_into`) or visit under the lock (`with_value`, which for values above
   the carrier returns `-EMSGSIZE` anyway).

Therefore **moving a value inside a pool and re-pointing `large_data`, done under
`settings_lock`, is invisible to every existing consumer.** Compaction is safe by
construction.

---

## Part A — upstream: shared large-value pool (`zmk-feature-custom-settings`)

### A.1 API

```c
/* Statically define a pool of _pool_size bytes that any number of
 * large-capable settings may share. Heap-free. */
#define ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(_name, _pool_size)                 \
    /* uint8_t _name_buf[_pool_size]; struct zmk_custom_setting_large_pool _name = {...}; */

struct zmk_custom_setting_large_pool {
    uint8_t *data;
    size_t size;
};

/* Like ZMK_CUSTOM_SETTING_DEFINE_SIZED, but the value's backing region is
 * allocated from _pool on demand instead of a dedicated static buffer.
 * max_size caps this one setting; the pool caps the sum over all members. */
#define ZMK_CUSTOM_SETTING_DEFINE_POOLED(_name, _max_size, _pool, _custom_subsystem_id, _key, \
                                         _value_type, _default_value, _confidentiality,      \
                                         _read_permission, _write_permission, _constraint)
```

Descriptor additions: `struct zmk_custom_setting_large_pool *large_pool;` (NULL for
non-pooled settings). Pooled settings start with `large_data == NULL, large_size == 0`.
`BUILD_ASSERT(_max_size <= CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE)` as for
`_SIZED`; `_max_size` may exceed `_pool_size`? No — assert `_max_size <= _pool_size`.

### A.2 Predicate & capacity changes

- `setting_uses_large_store(s)` → `(s->large_data != NULL || s->large_pool != NULL) &&
  (BYTES || STRING)`.
- `setting_value_capacity(s)` unchanged (`value_max_size`).
- Everywhere `large_store_set_raw` is about to store `size` bytes, a pooled setting
  first ensures a region of `size` (+1 for STRING's NUL) via the allocator below;
  `-ENOSPC` propagates out of `write_value_locked`/`write_bytes` (a new failure mode
  for writes — today's dedicated buffers can't fail this way).

### A.3 Allocator: contiguous regions + compact-on-demand (heap-free, O(pool) memmove)

No free lists, no headers inside the pool, no persisted state. A member's region is
described entirely by its own `large_data`/`large_size` (+1 for STRING). All under
`settings_lock`:

- **ensure_region(setting, new_size)**:
  1. If the setting already has a region and `new_size` fits its current extent
     (current region size ≥ new_size), write in place. (Shrink = write in place;
     `large_size` updates, slack is reclaimed at the next compaction.)
  2. Else: **compact** — enumerate all other members of this pool (walk
     `ZMK_CUSTOM_SETTING_FOREACH`, filter `large_pool == pool`, plus keyspace slots if
     pooled keyspaces are ever added; sort by `large_data`; slide each region down to
     eliminate gaps, updating each member's `large_data`). Then if
     `pool->size - Σ other members' extents ≥ new_size`, place this setting's region
     at the tail; else return `-ENOSPC` (caller's write fails cleanly, value
     unchanged — note: if the setting's *old* region was released as part of
     compaction planning, keep the old bytes valid until success is guaranteed:
     compute feasibility *before* moving anything, since sizes are all known).
  3. Empty value (`new_size == 0`): release the region (`large_data = NULL`).
- Writes are the only mutation sites; reads never allocate. Frequency is
  user-interactive (RPC edits, boot load), so an O(pool-size) memmove per growing
  write is negligible at the intended pool sizes (≤ a few KB).

### A.4 Interaction with existing paths (audit list for the implementer)

- **Defaults / init**: `apply_scalar_default_locked` on a pooled setting routes
  through ensure_region. Empty defaults (`size 0` — the runtime-macro case) allocate
  nothing, which is exactly the point.
- **settings_load (boot)**: `value_from_storage` → large path → ensure_region. On
  `-ENOSPC` (over-committed persisted data after a pool shrink across firmware
  versions), **skip the record with a warning** — same "reboot cannot fail" policy as
  keyspace bind-pool exhaustion.
- **discard**: re-reads flash into the setting → same ensure_region path.
- **reset**: back to default → empty ⇒ region released.
- **save**: reads current value, no allocation.
- **Temporary mode**: already `-EMSGSIZE` above the carrier for large-capable
  settings; pooled settings identical. No interaction with the pool.
- **Chunked RPC**: write staging assembles in the global staging buffer, commit goes
  through `write_bytes` → pooled path; read snapshots via `read_into`. No changes.
- **`sizeof` guarantees**: existing unit test asserts a normal setting keeps
  `large_data == NULL`; add the analogue for `large_pool`.

### A.5 Tests (upstream)

- Two pooled settings, budget smaller than the sum of their `max_size`s: fill one to
  near the pool, watch the second's write fail `-ENOSPC`; shrink the first; second now
  succeeds (exercises compaction).
- Grow/shrink/grow one member while another holds data → other member's bytes intact
  after pointer moves (read back around every step).
- Persist two pooled values → reboot-sim (`settings_load`) → both restored; then
  shrink the pool Kconfig… (not testable in one binary — instead simulate
  over-commit by writing a record for a third member directly into the fake settings
  backend and assert the skip-with-warning path).
- STRING pooled member keeps its NUL byte across compaction.

### A.6 Delivery

Stack a commit/PR on `codex/large-value-storage` (or fold into PR #24 if the owner
prefers — it reuses `setting_uses_large_store` seams that PR introduced). Per repo
rules, this can also be filed as an issue first; the spec above is intended to be
sufficient for a Sonnet session in that repo.

---

## Part B — `zmk-feature-runtime-macro` migration

Runtime-macro **does not currently build** against custom-settings `main` (it still
uses `ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE`, removed by PR #15), so this migration
is mandatory regardless of the size feature; this design is its vehicle.

### B.1 Data model & registrations (`src/runtime_macro.c`)

```
 names/<i> : N STRING scalars (ZMK_CUSTOM_SETTING_DEFINE)               -- replaces names array
 macros/<i>: N pooled BYTES scalars (ZMK_CUSTOM_SETTING_DEFINE_POOLED)  -- replaces bodies array
 (pool)    : ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(runtime_macro_pool,
             CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES)
 tap_ms    : INT32 scalar                                               -- unchanged
```

- **names stay plain (non-pooled, non-sized) scalars, NOT a P3 array** — post-P3,
  `zmk_custom_setting_set_default()` returns `-ENOTSUP` for array elements, which
  would break the DT-defaults name path and the "reset restores the DT name"
  semantics. `LISTIFY`-generated keys `"names/<i>"`.
- **bodies**: `LISTIFY`-generated `ZMK_CUSTOM_SETTING_DEFINE_POOLED(runtime_macro_body_##i,
  CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES, runtime_macro_pool, …, "macros/<i>",
  BYTES, ZMK_CUSTOM_SETTING_VALUE_BYTES(), …)`. Empty default ⇒ zero pool usage per
  empty slot. Storage names (`…/cormoran__runtime_macro/macros/<i>`) match the old
  array elements'.
- Resolve all name/body descriptors once at init into
  `static const struct zmk_custom_setting *names[COUNT], *bodies[COUNT]` (compile-time
  registrations ⇒ stable pointers; no arrays ⇒ no view-pool involvement anywhere).

### B.2 Kconfig

```kconfig
config ZMK_RUNTIME_MACRO_POOL_BYTES        # NEW — total shared body budget
    int "Shared runtime macro body pool size (bytes)"
    default 1024
    help
      Total bytes shared by all macro bodies. This is the only resident RAM
      added for large-macro support. The sum of all encoded bodies must fit;
      any single macro may still use up to ZMK_RUNTIME_MACRO_MAX_BYTES.

config ZMK_RUNTIME_MACRO_MAX_BYTES         # NEW — per-macro cap (bounds staging buffers)
    int "Maximum encoded bytes per runtime macro"
    default 256
    range ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE
    help
      Per-macro ceiling. Sizes the transient playback/RPC staging buffers, so
      keep it as small as your longest macro needs even if the pool is larger.
```

C-side `BUILD_ASSERT`s: `MAX_BYTES <= LARGE_VALUE_MAX_SIZE`,
`MAX_BYTES <= POOL_BYTES`. Users raise `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE`
(recommend 256) alongside; document in README. `QUEUE_SIZE` guidance as before (a
bigger body can decode to more queued items; `-ENOSPC` abort already handled).

### B.3 Storage layer

Public API (`zmk_runtime_macro_read/write/play/validate_encoded`) unchanged. All body
I/O via `read_into`/`write_bytes` — for large values the only firmware path; the
carrier-based `read_array_by_key` calls are deleted with the array.

- **write(index, name, encoded, size, persist)**: range-check; validate encoded;
  `size > MAX_BYTES ⇒ -EMSGSIZE`; `write_bytes(names[index], name, strlen, mode)`;
  `write_bytes(bodies[index], encoded, size, mode)`. **New failure mode:** body write
  can return `-ENOSPC` (pool budget exhausted) — surface it distinctly in the RPC
  handler ("macro pool full — delete or shrink another macro"), it is the design's
  user-visible trade-off.
- **read(index, …)**: `read_into(names[index], name, cap-1, &got, NULL)` +
  NUL-terminate (STRING `read_into` copies payload sans NUL);
  `read_into(bodies[index], encoded, cap, encoded_size, NULL)`.
- **Playback**: `player.encoded` grows to `MAX_BYTES`; gather is one
  `read_into(bodies[index], player.encoded, …)`; **delete `player.body_value`** (the
  resident 72 B carrier struct). Decode/queue unchanged.
- **save/discard/reset**: unchanged scope calls; Part A handles pool bookkeeping
  internally (discard/reset route through the same ensure_region path).

### B.4 Studio RPC (`proto/`, handler, `web/`)

Step-based protocol untouched; sizes change:

- `handle_list_macros`: `max_macro_bytes = CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES`.
  Consider also reporting pool occupancy (`pool_bytes_total`/`pool_bytes_used` in
  `MacroGlobalSettings`) so the web UI can show remaining budget — nice-to-have, and
  needs a small upstream getter (`zmk_custom_setting_large_pool_used(pool)`); include
  it in the Part A API if cheap.
- `.options`: `MacroSlot.steps max_count` 32 → 64 (worst case ≈ `MAX_BYTES/4` minimal
  binding steps at 256); bump `RUNTIME_MACRO_RPC_MAX_STEPS` to match. **Keep
  `KeyTapSequenceStep.packed_keys max_size:64`** — split longer packed-key runs into
  consecutive `KEY_TAP_SEQUENCE` ops (encoder side: handler `encode_steps` + DT
  defaults `flush_packed_sequence`; decode already enforces ≤ 64 per op).
- **RPC thread stack**: `steps[64]` ≈ 4.6 KB > default
  `CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE=4096` ⇒ raise to 8192 in
  `tests/zmk-config/build.yaml` + README.
- Handler whole-body buffers (`fill_macro_slot`, `handle_set_macro_name`,
  `read_macro_steps`, `write_macro_steps`, `handle_list_macros`) →
  `MAX_BYTES`, reading through the §B.3 helpers (carrier reads now `-EMSGSIZE` on
  large bodies). Name buffers stay carrier-sized.
- Frames: `SetMacroStep` stays small; existing `RX_BUF_SIZE=192` fine. Verify a
  64-step `GetMacroResponse` encodes through TX during the build test; if not, add
  paginated `GetMacroSteps` as a follow-up (not v1).
- Web (`web/src/macroCodec.ts`, `App.tsx`): honor dynamic `max_macro_bytes`, handle
  > 32 steps, mirror the ≤ 64 sequence-split rule; show a clear "pool full" error on
  `-ENOSPC` status; occupancy indicator if the optional field ships.

### B.5 DT default macros (`src/runtime_macro_dt_defaults.c`)

Names and bodies are plain scalars ⇒ `zmk_custom_setting_set_default()` works on both
as today; swap `find_array_element` → `find` with `"<prefix>/<slot>"` keys; keep the
file-owned static value storage (`set_default` stores a pointer).

**v1 limitation:** `set_default()` passes the 64-byte carrier even for large-capable
settings (PR #24's `set_default` copies the carrier into the large store). So **DT
default bodies stay ≤ 64 encoded bytes** in v1; runtime-written macros get the full
`MAX_BYTES`. Keep the 64 B encode buffer, keep the existing best-effort skip path, log
clearly ("DT default exceeds the 64-byte default limit"). **Upstream follow-up to
file** (do not block v1): `zmk_custom_setting_set_default_bytes(setting, data, size)`
holding pointer + size for large-capable settings; once it exists, DT defaults draw
from the pool like any other write (they consume budget — document that).

`SYS_INIT(..., APPLICATION, 91)` ordering unchanged (after `behavior_local_id_init`,
before `settings_load()`).

---

## 9. Memory budget (carrier struct ≈ 72 B; descriptor grows ~16 B under PR #24 + ~4 B for `large_pool`)

| Config | Resident body storage | Per-macro max | Total budget |
|---|---|---|---|
| **Today** (8 array elements) | 8 × 72 = **576 B** | 64 B | 512 B |
| v2 (8 × `DEFINE_SIZED`, 256) | 8 × 257 = **2056 B** | 256 B | 2048 B (fragmented per slot) |
| **v3 (this design)**: pool 1024, MAX 256 | **1024 B** (pool) | 256 B | 1024 B **shared** |
| v3, pool 2048, MAX 512 | 2048 B | 512 B | 2048 B shared |

v3 at the default config is **less resident RAM than v2** (1 KB vs 2 KB) while keeping
the same per-macro ceiling, *and* the budget is shared — ten short macros and one big
one coexist without per-slot waste. Empty slots cost zero pool bytes. Transient growth
(unchanged from v2): `player.encoded`, handler `encoded[]`/`steps[]`, RPC thread
stack; plus once per firmware, custom-settings' chunk/record staging at
`LARGE_VALUE_MAX_SIZE`.

---

## 10. Dependencies & rollout

1. **Part A lands first** in `zmk-feature-custom-settings` (stacked on
   `codex/large-value-storage` / PR #24, or folded in). File as issue → PR per that
   repo's rules.
2. Runtime-macro pins `west/west-dependency/west-dependency.yml` to the Part A branch
   until everything merges (same temporary-pin flow used for PR #11 earlier; revert to
   `main` after merge — leave a TODO in the PR body).
3. Re-verify the `DEFINE_SIZED`/`DEFINE_POOLED` argument order against the merged
   upstream before implementing Part B (PR #24 may change under review).
4. PR #25 (`codex/temp-slot-fix`) is adjacent, not a dependency (runtime-macro uses
   neither temporary mode nor cached views).
5. **If the pool extension is declined upstream**, fall back to v2 (N ×
   `DEFINE_SIZED`, kept fully specified in this document's git history) — everything
   in Part B except the registration macro and the `-ENOSPC` handling is identical.
   The single-blob alternative (§11) is the only module-local way to keep the shared
   budget, and is not recommended.

## 11. Rejected alternatives

- **Raise `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`.** Pinned to 64 by the protobuf
  schema; sizes every setting's union in every module.
- **v2: N per-slot `DEFINE_SIZED` settings.** Simple and upstream-aligned, but
  `COUNT × (MAX_BYTES+1)` resident even for empty slots — exactly the "no shared
  budget" property the owner asked to avoid. Kept as the documented fallback (§10.5).
- **v1: module-local 64 B chunk pool over P3 arrays** (chunk map per slot, free-set
  derived by scanning maps). Achieves the shared budget, but re-implements
  allocation/compaction concerns inside runtime-macro, needs a chunked DT-default
  installer, splits one logical value across many settings records, and duplicates
  the large-value mechanism PR #24 now owns upstream. The pool belongs where the
  bytes live.
- **Keyspace (P5b) with large `value_bufs`, one entry per macro.** Per-slot
  `max_entries × (max_size+1)` backing — same non-shared RAM as v2 — plus a full
  descriptor per slot and create/delete machinery whose named-entry capability is a
  *different* future feature (user-named slots replacing fixed `COUNT`). When that
  feature happens, pooled backing (Part A) composes with it upstream.
- **One big sized BYTES blob holding all bodies + internal directory (module-local).**
  Matches the pool's RAM but moves offset/compaction bookkeeping into runtime-macro,
  rewrites the whole blob to flash on every persist, and collapses per-macro
  save/discard granularity at the settings layer. Only worth it if Part A is declined
  and v2's RAM is unacceptable.
- **Chunked RPC (P6) as this module's transport.** Runtime-macro's step RPC never
  ships whole bodies; P6 matters for the generic settings UI only.

## 12. Testing

**Part A (upstream)**: §A.5.

**Part B unit (`tests/test`, native_sim):**
- Round-trip a > 64 B body write → read; bytes identical, size correct.
- Exactly `MAX_BYTES` ok; `MAX_BYTES + 1` ⇒ `-EMSGSIZE`.
- **Pool budget**: fill slots until a write fails `-ENOSPC`; delete one macro; the
  failed write now succeeds. Two macros: grow/shrink one, other's body intact
  (exercises compaction through this module's paths).
- Persist > 64 B macro, clobber in memory mode, discard ⇒ flash value restored.
- Playback of a > 64 B macro queues expected behaviors; over-`QUEUE_SIZE` decode
  aborts cleanly.
- `reset` restores DT default name and body (scalar `set_default` semantics).

**DT-defaults (`tests/dt-defaults`):** existing ≤ 64 B defaults still install; an
oversized `text` default is skipped with the clear log while others install.

**Build (`tests/zmk-config`):** add `-DCONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE=256`
and `-DCONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE=8192` to Studio targets; `test.py`
asserts `max_macro_bytes`=256 and pool presence. Optional hardware validation via the
`hw_test_dt_defaults` recipe: write a ~200 B macro over step RPCs, `GetMacro` back,
reboot, confirm persistence; then fill the pool and confirm the friendly `-ENOSPC`
error surfaces in the web UI.

**Web (`web/`):** codec tests for `max_macro_bytes`, > 32 steps, sequence splitting,
pool-full error rendering.

## 13. Implementation checklist

**Session 1 — custom-settings (Part A):**
1. `LARGE_POOL_DEFINE` + `DEFINE_POOLED` (+ optional `_used()` getter); descriptor
   `large_pool` field; predicate/capacity updates. (§A.1–A.2)
2. ensure_region with feasibility-before-move compaction; `-ENOSPC` plumbing;
   load-time skip-with-warning. (§A.3–A.4)
3. Unit tests §A.5; README section; stack on `codex/large-value-storage`; PR.

**Session 2 — runtime-macro (Part B):**
4. Pin west dep to the Part A branch (check merge state first). (§10)
5. `Kconfig`: `POOL_BYTES` (default 1024), `MAX_BYTES` (default 256). (§B.2)
6. `src/runtime_macro.c`: names → plain scalars `"names/<i>"`; bodies → pooled
   scalars `"macros/<i>"` + one pool; descriptor tables; rewrite read/write on
   `read_into`/`write_bytes`; `-ENOSPC` surfacing; grow `player.encoded`; delete
   `player.body_value`; `BUILD_ASSERT`s. (§B.1–B.3)
7. `src/runtime_macro_dt_defaults.c`: `find` instead of `find_array_element`; clear
   over-64 B log; file the upstream `set_default_bytes` issue. (§B.5)
8. proto/.options + handler: steps 64, buffers, sequence splitting,
   `max_macro_bytes`, optional pool occupancy. (§B.4)
9. `tests/zmk-config` config bumps + `test.py`; web updates; tests per §12;
   README. (§B.4, §12)
10. `pre-commit run`, `python3 -m unittest`, `cd web && npm test`; PR with the
    dependency-pin revert plan noted.
