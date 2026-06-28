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

The macro body stores behavior local IDs and two behavior params. Names are stored separately from bodies using [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings): `names[]` is an array string setting and `macros[]` is an array bytes setting under subsystem `cormoran__runtime_macro`.

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

Open the Web UI from ZMK Studio custom subsystem list, connect over serial, select a macro slot, edit steps, then use **Write Memory** for a temporary update or **Save** for persistent storage. The RPC protocol sends names, step count, each step, and delete operations with separate write requests to keep every request body small.

The runtime macro RPC also exposes `MacroGlobalSettings`, currently containing `tap_ms`. The get request returns the whole global settings message so future global settings can be added together; writes are per key, such as `set_tap_ms`.

## Binary Format

Each stored macro body is a byte array:

```text
version: u8 = 1
steps...
```

Step fields use unsigned base-128 varints.

```text
down:  opcode=1, behavior_id, param1, param2
up:    opcode=2, behavior_id, param1, param2
tap:   opcode=3, behavior_id, param1, param2
delay: opcode=4, delay_ms
```

The default custom-settings value size is 64 bytes, so the UI reports the encoded byte size before saving.

## Development

```bash
pre-commit run
python3 -m unittest
west zmk-build tests/zmk-config
west zmk-test tests -m .
cd web && npm test
```

The Web UI can import and export the macro step subset from the Keyboard Abyss keybindings schema. Runtime behavior bindings are represented as valid `raw` bindings using `local-id:<behavior_id> <param1> <param2>`.
