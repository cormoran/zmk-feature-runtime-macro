# Keyspace macros: rebuilding storage on zmk-feature-custom-settings' keyspaces

Status: **implemented** (branch `claude/keyspace-macros`, supersedes
[large-macros-shared-pool.md](large-macros-shared-pool.md) and PR #11).
Backward compatibility (stored data and RPC/proto) is **intentionally broken**
per the owner's explicit direction - see [Breaking changes](#breaking-changes).

## 1. Motivation

zmk-feature-custom-settings' simplification pass
(`docs/design/simplification-redesign.md` in that repo, §5) rebuilt keyspaces
into a lean, RPC-creatable, name-addressed entry mechanism: an entry is one
opaque `[user_key\0][payload]` pooled BYTES value, created/deleted via
`zmk_custom_setting_keyspace_create`/`_delete`, and already covered by the
generic Studio `CreateSetting`/`DeleteSetting`/`ListSettings`/`GetSetting` RPC.

This is exactly the capability runtime-macro's pre-existing design
(large-macros-shared-pool.md) built by hand: `CONFIG_ZMK_RUNTIME_MACRO_COUNT`
fixed slots, each with its own `names.<i>` / `macros.<i>` settings and a
shared body pool. Rebuilding on the keyspace primitive lets this module
delete essentially all of that plumbing - fixed-slot LISTIFY registration,
the separate name/body split, the DT-default `set_default()` carrier
juggling - and gain **named** macros (rather than numbered slots) as a side
effect, which is also a better user experience.

## 2. The new model: a macro IS a keyspace entry

```c
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE(
    runtime_macros, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, "macro/",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    /* max_size */ CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES,
    /* max_key_len */ RUNTIME_MACRO_MAX_KEY_LEN,
    /* max_entries */ CONFIG_ZMK_RUNTIME_MACRO_COUNT,
    /* pool_size */ CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES,
    ...);
```

- A macro's **name** is the keyspace key suffix ("macro/hello"); its
  **payload** is the encoded step body (same binary format as before,
  unchanged). The `names.<i>` settings, the module-owned pool, and both
  LISTIFY blocks from the old design are gone.
- Create/delete/rename go through
  `zmk_custom_setting_keyspace_create`/`_delete` (rename = create-new +
  delete-old, see `zmk_runtime_macro_rename`). The generic custom-settings
  Studio RPC (`CreateSetting`/`DeleteSetting`/`ListSettings`/`GetSetting`/
  `WriteValueChunk`) already covers raw entry management, so this module's
  own RPC no longer needs `SetMacroName`/`DeleteMacro` at all.
- Persistence and reboot re-binding are entirely upstream's job now
  (ordinal `"<subsystem>/macro/#<i>"` records bind slot `i` back on
  `settings_load()`). This module's code never touches storage names
  directly.
- **Payload access** (`zmk_custom_setting_read_into`,
  `zmk_custom_setting_write_bytes`, `zmk_custom_setting_public_key`) is
  already keyspace-aware: it sees/returns the payload for read/write, and
  the full `"macro/<name>"` key for `public_key` (stripped back to the bare
  name by this module's `strip_prefix` helper). Verified directly against
  upstream's `custom_settings.c` keyspace interception code, and exercised
  by the very first tests in `src/test/runtime_macro_test.c`.
- **Keymap binding**: the behavior keeps its numeric param as the keyspace
  **slot index** (0..`CONFIG_ZMK_RUNTIME_MACRO_COUNT - 1`). Slot indices are
  stable across reboots for a given persisted entry (ordinal persistence
  re-binds record `#i` to slot `i` every boot), but are assigned by the
  keyspace (first free slot) at create time, not chosen by the caller - so
  a macro's slot is discovered, not predicted. `zmk_runtime_macro.h`
  exposes `zmk_runtime_macro_name_for_slot`/`_slot_for_name` for this
  resolution, backed directly by the keyspace's public
  `slots[max_entries]` array (no new upstream accessor was needed - see
  [Upstream enablers](#upstream-enablers)). An unbound/empty slot plays
  nothing (logged at debug), not an error.

## 3. RPC/proto redesign

Everything macro-identifying now carries a `name` (string) instead of an
`index` (uint32), and list/get responses add a `slot` (uint32) field so
clients can learn/display the slot-to-bind. Dropped in favor of the generic
custom-settings RPC: `SetMacroNameRequest`, `DeleteMacroRequest` (tags 3 and
8, `reserved`). Kept, retargeted to `name`: `GetMacroRequest`,
`SetMacroStepCountRequest`, `SetMacroStepRequest`. Added:
`AppendMacroStepRequest` (append one step without a separate get-count
round trip - a small ergonomic win the design brief invited). `MacroSlot`
was renamed `MacroDetail` (a "slot" is now a resolved *property* of a
macro, not its primary identity, so naming the whole detail message after
it was confusing). `MacroGlobalSettings.max_macro` was renamed
`max_entries` to describe what it actually bounds post-keyspace (concurrent
macro count, independent of the byte pool).

The Web UI now creates/deletes/renames macros via
`cormoran_custom_settings`' generic `CreateSetting`/`DeleteSetting` RPC
(vendored a copy of that module's `.proto` into `web/vendor-proto/` purely
for TS codegen - kept out of this module's own `proto/` directory so the
firmware build's `proto/*.proto` glob never sees it and tries to compile a
conflicting duplicate of the real dependency's nanopb types). A rename
composes create-under-new-name + delete-old-name client-side, using the
body bytes already loaded in the editor (no extra RPC round trip needed).

## 4. DT default macros: seed-if-absent, not `set_default()`

The old design (608 lines in `runtime_macro_dt_defaults.c`) called
`zmk_custom_setting_set_default()` on each of a slot's two settings at
`SYS_INIT(APPLICATION, 91)` - carefully ordered to run after
`behavior_local_id_init` (default priority 90) so behavior names resolve,
but before `settings_load()` (always later, from `main()`). It also
inherited a real limitation: `set_default()` only accepted the 64-byte
carrier, so a DT-default body couldn't use the module's full
`MAX_BYTES` ceiling the way a Web UI-written macro could.

The new design has no `default_value` concept for keyspace entries at all
(a slot's blob always carries its own key, so there's no shared static
default that could represent a not-yet-existing entry - see
zmk-feature-custom-settings' P3 implementation notes). Instead:

- This module registers its own `SETTINGS_STATIC_HANDLER_DEFINE` whose
  `h_commit` callback is `runtime_macro_seed_dt_defaults()`. Zephyr's
  settings subsystem calls every registered handler's `h_commit` once
  `settings_load()` has finished loading **every** persisted record from
  **every** handler (confirmed by reading `subsys/settings/src/
  settings_store.c`: `settings_load_subtree` loads all sources, *then*
  calls `settings_commit_subtree`) - so by the time this callback runs,
  every persisted macro is already bound to its slot.
- For each DT default whose name has no live keyspace entry yet
  (`zmk_custom_setting_keyspace_find` returns NULL), it calls
  `zmk_runtime_macro_create(name, encoded_body, ..., MEMORY mode, NULL)`.
  `-EEXIST` (a persisted macro already has this name) is treated as
  success - the persisted value wins, unconditionally.
- Because this runs after `settings_load()` - which itself always runs
  after every `SYS_INIT` stage, including `behavior_local_id_init` - the
  old SYS_INIT-ordering fragility (the `91` literal hack) is gone entirely.
  The encoder can resolve behaviors at seed time with no special ordering
  care.
- Semantics change (breaking, documented in the README): a DT default is
  seeded fresh every boot in MEMORY mode, never persisted by the seed step
  itself. Deleting one from the Web UI is a per-session removal - it
  returns on the next reboot, exactly like resetting a factory value would
  under the old design's `set_default()`/`reset()` semantics. To remove a
  DT default permanently, remove its node from the devicetree. Editing and
  **Save**-ing a DT default from the Web UI creates a real persisted macro
  under that name, which then wins on every subsequent boot (the seed step
  only fires when no live entry exists).
- The devicetree binding dropped its `slot` property entirely (a keyspace
  entry's slot isn't chooseable at create time - see below) and renamed
  `display-name` to `macro-name` (also: devicetree reserves a bare `name`
  property for legacy node-name mirroring; `dtc` rejects any other value
  for it, which is why the property isn't simply called `name`). DT
  defaults are now seeded in devicetree declaration order into the first
  available slots - stable in practice on a device with no persisted
  macros, but not a documented guarantee (see the README's "Devicetree
  Default Macros" section for the precise caveat).

## 5. Upstream enablers

Two enablers were anticipated by the task brief; only one was needed.

**A. Keyspace pool-budget override (needed, implemented).**
`ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE` sized its pool at the worst case,
`max_entries * (max_key_len + max_size)` - exactly the property
large-macros-shared-pool.md's shared body pool was designed to avoid.
Added `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE` (a sibling entry
point taking an explicit `pool_size`, funneling into a new
`_WITH_POOL_SIZE_RPC_CONVERTERS_AND_CONSTRAINTS` innermost macro; the
existing macros stay source-compatible sugar that compute the worst case
and forward to the same innermost macro) plus a `BUILD_ASSERT` that the
budget fits at least one full-size entry, landed as commit `3586d42` on
zmk-feature-custom-settings' `codex/simplify-p4-descriptor` branch (stacked
on PR #31). A new native test, `test_keyspace_pool_overcommit`, proves
`-ENOSPC` triggers well before `max_entries` slots are used (8 slots, a
pool sized for 3 full-size entries - creation stops at 4, not 8).
runtime-macro uses this to restore its pre-migration shared-pool budget
property: `CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES` (default 1024) instead of
`COUNT * MAX_BYTES` (2048 at the same defaults).

**B. Slot-by-index accessor (anticipated, not needed).** The task brief
allowed for a small upstream addition to resolve a keyspace's slots by
index if no public way existed. It turned out
`struct zmk_custom_setting_keyspace`'s `slots[max_entries]` array and
`max_entries` field are already public in the header (needed for the
generic RPC handler's own list/save/discard scope-application passes), so
`runtime_macro.c` reads `runtime_macros.slots[i].in_use` /
`&runtime_macros.slots[i].setting` directly - no upstream change required.

## 6. Gotchas resolved by testing, not assumption

- **All-digit macro names** (e.g. `"42"`, key `"macro/42"`): the old design
  had a real collision with custom-settings' load-time
  `split_array_element_key` parser, which treated any stored
  `"<text>/<digits>"` name as a legacy array element - the reason the old
  `names.<i>`/`macros.<i>` keys used `.` instead of `/` as their index
  separator. Investigation of the current (P3/P4) `custom_settings.c`
  shows `zmk_custom_setting_find` (the general lookup used by name-based
  create/find/read/write) never calls that parser at all - it's confined to
  `custom_settings_handle_set`, the settings-*load* callback, which for
  keyspaces only ever sees ordinal storage names (`"macro/#3"`, safe: `#`
  is never a digit). A user-facing key like `"macro/42"` is never used as a
  *storage* name (only the ordinal is), so the collision doesn't apply
  post-migration. Verified directly with `test_all_digit_name` in
  `src/test/runtime_macro_test.c` (create/read/delete round-trip for name
  `"42"`) rather than assumed from the code reading above.
- **`WriteMode` on a rename's delete half**: `zmk_custom_setting_keyspace_delete`
  has no mode parameter (it always erases the persisted record if any,
  matching the generic `DeleteSettingRequest`'s shape) - `zmk_runtime_macro_delete`
  mirrors that (no mode parameter), and `zmk_runtime_macro_rename` only
  takes a mode for its create half.
- **`STRUCT_SECTION_ITERABLE`-based keyspace object is public**: this let
  the module skip adding any new accessor API (§5, enabler B) but also
  means `runtime_macros` (the keyspace variable) is a linker symbol other
  code in this module can reference directly - kept `static` was not an
  option (the macro doesn't support that), so this is a soft internal
  contract, not a hard one; only `src/runtime_macro.c` and
  `src/runtime_macro_dt_defaults.c` (indirectly, through the public
  `zmk_runtime_macro_*` API) touch it in this module.

## 7. Breaking changes

- **Storage**: previously-persisted `names.<i>`/`macros.<i>` records are
  silently dropped on upgrade (different storage names entirely - ordinal
  `"<subsystem>/macro/#<i>"` now). No migration path is provided (explicitly
  out of scope per the owner).
- **Firmware API** (`include/cormoran/zmk/runtime_macro.h`): every
  index-addressed function (`zmk_runtime_macro_read`/`_write`) is now
  name-addressed; `zmk_runtime_macro_play` keeps its slot-index parameter
  (unchanged signature) since that's the keymap behavior's own contract.
  New: `_create`, `_delete`, `_rename`, `_name_for_slot`, `_slot_for_name`,
  `_for_each`.
- **RPC/proto**: see §3. `SetMacroName`/`DeleteMacro` removed;
  `index` renamed `name` (string) throughout; `MacroSlot` renamed
  `MacroDetail`; `max_macro` renamed `max_entries`; `slot` (uint32) added
  to list/get responses; `AppendMacroStepRequest` added.
- **Devicetree binding**: `cormoran,runtime-macro-default`'s `slot`
  property is removed (a keyspace entry's slot is assigned at create time,
  not chosen); `display-name` is renamed `macro-name`.

## 8. Line-count delta

See the PR description for the exact `wc -l` before/after `src/`; the net
effect is a large reduction, driven mostly by deleting
`runtime_macro_dt_defaults.c`'s `slot_used[]`/duplicate-detection
bookkeeping (no longer needed - names are the identity, and
`zmk_custom_setting_keyspace_create` already rejects a duplicate key with
`-EEXIST`) and the two LISTIFY blocks + descriptor-pointer tables in
`runtime_macro.c`.
