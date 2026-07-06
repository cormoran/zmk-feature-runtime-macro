# ZMK Runtime Macro

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)

Runtime Macro is a ZMK module that lets you edit small macros at runtime through the unofficial custom ZMK Studio RPC protocol.

Macros are stored by number and invoked from keymaps with:

```dts
&rmacro 0
```

Each macro has a display name for the Web UI and a compact binary body. The body supports:

- behavior down
- behavior up
- behavior tap using the global `tap_ms` setting
- delay in milliseconds
- packed `&kp` tap sequence using the global `tap_ms` setting

The macro body stores behavior local IDs and two behavior params. Names and bodies are each stored as one [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings) setting per slot (`names.0`, `names.1`, ..., `macros.0`, `macros.1`, ...) under subsystem `cormoran__runtime_macro`. Every macro body shares one RAM pool - see [Macro Size Limits](#macro-size-limits) below.

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

Open the Web UI from ZMK Studio custom subsystem list, connect over serial, select a macro slot, edit steps, then use **Write Memory** for a temporary update or **Save** for persistent storage. Memory updates become pending custom setting changes; use **Save Pending** to persist all pending runtime macro changes, or **Discard Pending** to restore the saved values. The RPC protocol sends names, step count, each step, and delete operations with separate write requests to keep every request body small.

The runtime macro RPC also exposes `MacroGlobalSettings`, containing `tap_ms` and `max_macro`. The get request returns the whole global settings message so future global settings can be added together; writes are per key, such as `set_tap_ms`. `max_macro` is read-only and reports the configured maximum macro slot count.

## Devicetree Default Macros

By default every macro slot starts empty, so a freshly flashed board needs a Studio connection before `&rmacro` does anything. To ship a macro that already works out of the box, give a slot a **devicetree default**: a factory value defined at compile time in your `.keymap`/`.overlay`, with no Studio connection required and no extra Kconfig option to enable.

Add one `cormoran,runtime-macro-default` node per slot, anywhere under `/ { ... }` (it doesn't need to live inside `keymap` or `behaviors`). The simplest case just types text:

```dts
/ {
    runtime_macro_defaults {
        rmacro_default_0 {
            compatible = "cormoran,runtime-macro-default";
            slot = <0>;
            display-name = "Email";
            text = "user@example.com";
        };
    };
};
```

With `&rmacro 0` bound somewhere in your keymap, pressing it types `user@example.com` immediately after flashing - before ever opening the Web UI.

For steps other than plain text - modifier holds, other behaviors, explicit delays - use `bindings`, which reuses ZMK's native macro vocabulary so there's nothing new to learn. This example (added as another node next to `rmacro_default_0` above, inside the same `runtime_macro_defaults` container) holds Shift for one key, then taps `w` and `Enter` (handy as a Vim `:w<Enter>` save macro bound next to a "Vim mode" layer):

```dts
rmacro_default_1 {
    compatible = "cormoran,runtime-macro-default";
    slot = <1>;
    display-name = "Vim save";
    bindings = <&macro_press &kp LSHFT>
             , <&macro_tap &kp SEMI>
             , <&macro_release &kp LSHFT>
             , <&macro_wait_time 5>
             , <&macro_tap &kp W>
             , <&kp RET>;
};
```

Each comma-separated entry is one binding, same as a keymap's `bindings` list. `&macro_tap` / `&macro_press` / `&macro_release` switch the mode applied to the entries that follow (starting mode is tap) and stay in effect until changed again - the trailing `<&kp RET>` above is still a tap because mode was last set to tap. `&macro_wait_time <ms>` inserts an explicit delay. Any other behavior binding (`&kp`, `&mo`, ...) is encoded using its behavior local ID, and plain-ASCII `&kp` taps made while in tap mode are packed exactly like `text`, so mixing `text` and `bindings` in the same slot (`text` is always encoded first) doesn't cost extra bytes.

| property       | meaning                                                                                        |
| -------------- | ----------------------------------------------------------------------------------------------- |
| `slot`         | Target macro slot index (`0` to `CONFIG_ZMK_RUNTIME_MACRO_COUNT - 1`), required.                  |
| `display-name` | Name shown in the Web UI. Defaults to the node name.                                              |
| `text`         | Plain ASCII text, encoded first as a packed key-tap sequence (same encoding the Web UI uses).      |
| `bindings`     | ZMK-native macro steps, encoded after `text`.                                                      |
| `wait-ms`      | Delay inserted between consecutive non-packed `bindings` steps. Default `0` (none).                |

At least one of `text` or `bindings` must be set.

A default behaves like a factory value, not a one-time seed: it shows up in the Web UI like any other macro, editing and saving it writes a normal user value that shadows the default, and **Discard Pending** / resetting the setting / erasing settings all bring the devicetree default back. If a default fails to encode (for example an unsupported character in `text`, or a `slot` that doesn't fit `CONFIG_ZMK_RUNTIME_MACRO_COUNT`), that one slot is skipped and logged - it does not fail the rest of the build.

**Devicetree defaults are limited to 64 encoded bytes**, smaller than the `CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES` limit that applies to macros written from the Web UI - see [Macro Size Limits](#macro-size-limits). A `text`/`bindings` default that encodes past 64 bytes is skipped with a log at boot ("DT default exceeds the 64-byte default limit") rather than failing the build; keep devicetree defaults short, and use the Web UI for longer macros.

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

Every macro body draws from **one shared RAM pool** sized by `CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES` (default 1024 bytes), instead of each of the `CONFIG_ZMK_RUNTIME_MACRO_COUNT` slots reserving its own worst-case buffer. A short macro (or an empty, unused slot) costs close to zero pool bytes; a long one can use much more, up to a per-macro ceiling:

| Kconfig                                    | Meaning                                                                                      | Default |
| ------------------------------------------- | --------------------------------------------------------------------------------------------- | ------- |
| `CONFIG_ZMK_RUNTIME_MACRO_POOL_BYTES`       | Total bytes shared by every macro body combined.                                              | 1024    |
| `CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES`        | Largest a single macro's encoded body may be. Also sizes playback/RPC staging buffers.         | 256     |
| `CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE` (from zmk-feature-custom-settings) | Must be raised to at least `MAX_BYTES` (see the `.conf` snippet above) - it caps every large/pooled setting in the firmware, not just this module's. | 64 |

Raise `POOL_BYTES` if you want many long macros to coexist; raise `MAX_BYTES` (and `LARGE_VALUE_MAX_SIZE` alongside it) if a single macro needs to be longer than 256 bytes. Keep `MAX_BYTES` no larger than you actually need - it sizes the transient playback buffer and the Studio RPC step-staging buffers even for short macros. A longer macro can also decode into more queued key/behavior events than `CONFIG_ZMK_RUNTIME_MACRO_QUEUE_SIZE` (default 64) allows during playback; raise it alongside `MAX_BYTES` if a very long macro's playback aborts with a "queue failed" log.

If the pool is full, saving or updating a macro body fails and the Web UI shows **"Macro pool full: delete or shrink another macro"** - the macro's previous value is left untouched. The Web UI also shows the pool's current usage (e.g. "Shared macro pool: 300/1024 B used") next to the macro list.

Devicetree defaults have a separate, smaller limit - see the note in [Devicetree Default Macros](#devicetree-default-macros).

## Development

```bash
pre-commit run
python3 -m unittest
west zmk-build tests/zmk-config
west zmk-test tests -m .
cd web && npm test
```

The Web UI can import and export the macro step subset from the Keyboard Abyss keybindings schema. Runtime behavior bindings are represented as valid `raw` bindings using `local-id:<behavior_id> <param1> <param2>`.
