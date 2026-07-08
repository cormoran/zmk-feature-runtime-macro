# ZMK Runtime Macro

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)

Runtime Macro is a ZMK module that lets you create and edit small macros at runtime through the unofficial custom ZMK Studio RPC protocol.

Macros are **created and addressed by name** (e.g. `"hello"`, stored as the [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings) keyspace entry `macro/hello` under subsystem `cormoran__runtime_macro`). A keymap binding, however, only takes a **number**:

```dts
&rmacro 0
```

That number is the macro's **slot index** - a stable position (0 to `CONFIG_ZMK_RUNTIME_MACRO_COUNT - 1`) assigned when the macro is created and unchanged across reboots. It is **not** something you choose ahead of time: after creating a macro, check the Web UI's macro list (or the `ListMacros` RPC response) to see which slot it landed in, then bind that number in your keymap.

Each macro's body is a compact binary blob supporting:

- behavior down
- behavior up
- behavior tap using the global `tap_ms` setting
- delay in milliseconds
- packed `&kp` tap sequence using the global `tap_ms` setting

Every macro's name + body together share one RAM pool - see [Macro Size Limits](#macro-size-limits) below.

Tap duration is a scalar custom setting, `tap_ms`, in the same subsystem. If one step needs a different duration, encode it as down, delay, then up.

## User Guide

Add the module to `config/west.yml`.

```yml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-runtime-macro
      remote: cormoran
      revision: main
      import: true
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

Enable the module and Studio RPC in `config/<shield>.conf`.

```conf
CONFIG_ZMK_RUNTIME_MACRO=y
CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_CRC16=y
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_RUNTIME_MACRO_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=192
CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES=192
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
# Required alongside the defaults below (see "Macro Size Limits"):
CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE=256
CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE=8192
```

Include the behavior definition and bind a macro slot in your keymap.

```dts
#include <behaviors.dtsi>
#include <behaviors/runtime_macro.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";

        default_layer {
            bindings = <
                &rmacro 0
            >;
        };
    };
};
```

An unbound/empty slot plays nothing - so `&rmacro 0` is harmless on a freshly flashed board with no macros created yet.

Open the Web UI from the ZMK Studio custom subsystem list, connect over serial, then:

1. **Create** a macro by typing a name and clicking **Create** - it appears in the macro list with the slot number it was assigned.
2. Edit its steps, then use **Write Memory** for a temporary update or **Save** for persistent storage.
3. Bind that macro's slot number (`&rmacro <slot>`) in your keymap.
4. **Rename** or **Delete** a macro any time from the editor.

Memory updates become pending custom setting changes; use **Save Pending** to persist all pending runtime macro changes, or **Discard Pending** to restore the saved values.

Create/Delete/Rename go through zmk-feature-custom-settings' generic `CreateSetting`/`DeleteSetting` RPC (subsystem `cormoran_custom_settings`, key `macro/<name>`) rather than a runtime-macro-specific request - this module's own RPC only carries macro-domain operations (step-level editing, listing, playback-adjacent global settings) that the generic RPC can't express. A rename is a create-under-the-new-name followed by a delete-of-the-old-name, so a failure partway through never loses the macro's content.

The runtime macro RPC also exposes `MacroGlobalSettings`, containing `tap_ms` and `max_entries`. The get request returns the whole global settings message so future global settings can be added together; writes are per key, such as `set_tap_ms`. `max_entries` is read-only and reports the configured maximum number of macros that can exist at once (`CONFIG_ZMK_RUNTIME_MACRO_COUNT`).

## Devicetree Default Macros

By default no macros exist, so a freshly flashed board needs a Studio connection before `&rmacro` does anything anywhere. To ship a macro that already works out of the box, declare a **devicetree default**: a factory macro defined at compile time in your `.keymap`/`.overlay`, with no Studio connection required and no extra Kconfig option to enable.

Add one `cormoran,runtime-macro-default` node per macro, anywhere under `/ { ... }` (it doesn't need to live inside `keymap` or `behaviors`). The simplest case just types text:

```dts
/ {
    runtime_macro_defaults {
        rmacro_default_0 {
            compatible = "cormoran,runtime-macro-default";
            macro-name = "Email";
            text = "user@example.com";
        };
    };
};
```

After flashing, open the Web UI (or the `ListMacros` RPC) to see which slot `"Email"` was assigned, then bind `&rmacro <that slot>` in your keymap - pressing it types `user@example.com` immediately, before ever opening the Web UI to edit it.

For steps other than plain text - modifier holds, other behaviors, explicit delays - use `bindings`, which reuses ZMK's native macro vocabulary so there's nothing new to learn. This example (added as another node next to `rmacro_default_0` above, inside the same `runtime_macro_defaults` container) holds Shift for one key, then taps `w` and `Enter` (handy as a Vim `:w<Enter>` save macro):

```dts
rmacro_default_1 {
    compatible = "cormoran,runtime-macro-default";
    macro-name = "Vim save";
    bindings = <&macro_press &kp LSHFT>
             , <&macro_tap &kp SEMI>
             , <&macro_release &kp LSHFT>
             , <&macro_wait_time 5>
             , <&macro_tap &kp W>
             , <&kp RET>;
};
```

Each comma-separated entry is one binding, same as a keymap's `bindings` list. `&macro_tap` / `&macro_press` / `&macro_release` switch the mode applied to the entries that follow (starting mode is tap) and stay in effect until changed again - the trailing `<&kp RET>` above is still a tap because mode was last set to tap. `&macro_wait_time <ms>` inserts an explicit delay. Any other behavior binding (`&kp`, `&mo`, ...) is encoded using its behavior local ID, and plain-ASCII `&kp` taps made while in tap mode are packed exactly like `text`, so mixing `text` and `bindings` in the same macro (`text` is always encoded first) doesn't cost extra bytes.

| property     | meaning                                                                                          |
| ------------- | ------------------------------------------------------------------------------------------------- |
| `macro-name`  | The macro's name (its keyspace key, `macro/<name>`). Defaults to the node name. Must be unique.    |
| `text`        | Plain ASCII text, encoded first as a packed key-tap sequence (same encoding the Web UI uses).      |
| `bindings`    | ZMK-native macro steps, encoded after `text`.                                                      |
| `wait-ms`     | Delay inserted between consecutive non-packed `bindings` steps. Default `0` (none).                |

At least one of `text` or `bindings` must be set.

**A devicetree default is seeded, not persisted.** On every boot, after settings load, each `cormoran,runtime-macro-default` node whose name has no live macro yet is created in memory (not saved to flash). This means:

- Editing a devicetree default's body from the Web UI and clicking **Save** creates a real, persisted macro under that name - from then on, the persisted value wins every boot (the seed step only fires when no macro of that name already exists).
- **Deleting** a devicetree default from the Web UI removes it only for the current session - it comes back, un-persisted, on the next reboot. To remove a devicetree default permanently, remove its node from the devicetree instead.
- A devicetree default's assigned slot is **not guaranteed to be the same every boot** if other macros are created/deleted around it - it depends on which slots are already occupied by persisted macros at boot time. On a device with no persisted macros, devicetree defaults are seeded in devicetree declaration order into the first available slots, which is stable in practice; always double check via the Web UI/RPC list rather than assuming a fixed number.
- A default that fails to encode or exceeds `CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES` (for example an unsupported character in `text`, or too much `bindings` content) is skipped with a log at boot - it does not fail the build, and does not prevent the other defaults from installing.

## Binary Format

Each stored macro body is a byte array:

```text
version: u8 = 1
steps...
```

Step fields use unsigned base-128 varints.

```text
down:         opcode=1, behavior_id, param1, param2
up:           opcode=2, behavior_id, param1, param2
tap:          opcode=3, behavior_id, param1, param2
delay:        opcode=4, delay_ms
key sequence: opcode=5, byte_length, packed_key_bytes...
```

The key sequence opcode is optimized for consecutive `&kp` taps that use HID keyboard usages with no modifier or left shift. Each packed key byte uses bit 7 for left shift and bits 0-6 for the HID keyboard usage ID, so common ASCII-producing taps cost one byte per key plus the opcode and length bytes. During playback each packed key is expanded to a normal `&kp <keycode>` tap using the global `tap_ms` value.

## Macro Size Limits

Every macro's name + body together draw from **one shared RAM pool** sized by `CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES` (default 1024 bytes), instead of each of the `CONFIG_ZMK_RUNTIME_MACRO_COUNT` slots reserving its own worst-case buffer. A short macro costs close to its actual name+body bytes; a long one can use much more, up to a per-macro ceiling:

| Kconfig                                                                              | Meaning                                                                                                                                                | Default |
| ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- | ------- |
| `CONFIG_ZMK_RUNTIME_MACRO_COUNT`                                                     | How many macros can exist at once (bounds the fixed slot-descriptor array, independent of the byte pool below).                                        | 8       |
| `CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN`                                              | Longest a macro's name may be, in bytes.                                                                                                                | 24      |
| `CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES`                                                | Total bytes shared by every macro's name + body combined.                                                                                               | 1024    |
| `CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES`                                                 | Largest a single macro's encoded body may be. Also sizes playback/RPC staging buffers.                                                                  | 256     |
| `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE` (from zmk-feature-custom-settings) | Must be raised to at least `MAX_BYTES` (see the `.conf` snippet above) - it caps every large/pooled setting in the firmware, not just this module's.    | 64      |

Raise `POOL_BYTES` if you want many long macros to coexist; raise `MAX_BYTES` (and `LARGE_VALUE_MAX_SIZE` alongside it) if a single macro needs to be longer than 256 bytes. Keep `MAX_BYTES` no larger than you actually need - it sizes the transient playback buffer and the Studio RPC step-staging buffers even for short macros. A longer macro can also decode into more queued key/behavior events than `CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE` (default 64) allows during playback; raise it alongside `MAX_BYTES` if a very long macro's playback aborts with a "queue failed" log.

If the pool is full, creating or updating a macro fails and the Web UI shows **"Macro pool full: delete or shrink another macro"** - the macro's previous value is left untouched. The Web UI also shows the pool's current usage (e.g. "Shared macro pool: 300/1024 B used") next to the macro list. Raising `CONFIG_ZMK_RUNTIME_MACRO_COUNT` costs a small, fixed amount of RAM per extra slot regardless of the pool budget - it does not affect how many bytes of macro content fit.

## Upgrading from a pre-keyspace release

This module's macro storage was rebuilt on [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings)' keyspace feature (see [`docs/design/keyspace-macros.md`](docs/design/keyspace-macros.md)). This is a **breaking storage-schema and RPC change**:

- Previously-recorded macros (stored per numeric slot as `names.<i>` / `macros.<i>`) are **not migrated** and are silently dropped on upgrade - recreate your macros after flashing.
- Macros are now created/deleted/renamed by **name**, not by a fixed slot index; a keymap binding's numeric parameter is now an assigned slot you look up after creating a macro, not a slot you write directly into.
- The RPC protocol dropped `SetMacroName`/`DeleteMacro` (superseded by generic `CreateSetting`/`DeleteSetting`) and renamed some fields (`index` → `name`/`slot`, `max_macro` → `max_entries`, `MacroSlot` → `MacroDetail`) - regenerate any custom RPC client against the new `.proto`.

## Development

```bash
pre-commit run
python3 -m unittest
west zmk-build tests/zmk-config
west zmk-test tests -m .
cd web && npm test
```

The Web UI can import and export the macro step subset from the Keyboard Abyss keybindings schema. Runtime behavior bindings are represented as valid `raw` bindings using `local-id:<behavior_id> <param1> <param2>`.
