This repository contains a ZMK module with Web UI using the **unofficial** custom ZMK Studio RPC protocol.

## Initialization (first time only)

This repo is created from template. Run the following to find all places that need to be replaced:

```
rg '(t|T)emplate'
rg 'your-name|your_name'
```

Key things to replace:

- Rename `proto/your-name/template/template.proto` and `src/studio/template_handler.c` with your feature name, and update all references found by the search above.
- Replace both `your-name` and `template` placeholders. `your-name` is the placeholder for the module author's namespace.
- Use `<your name>__<module name>` for the custom Studio subsystem/module identifier to avoid conflicts. In C identifiers and protobuf package names, use an identifier-safe form like `your_name__template` / `your_name.template`.
- Use `<your name>/<module name>/` for include paths and protobuf file paths to avoid conflicts, such as `proto/your-name/template/` and `#include <your-name/template/template.pb.h>`.
- Update `zephyr/module.yml`: change the module name.
- Update `README.md`: replace template descriptions with your module's description.
- Update `web/vite.config.ts`: change `base` to your repository name.

Remove this "Initialization" section from AGENTS.md (CLAUDE.md is symlink) after completing all items.

## Dev Rules

- Commit changes at each milestone. Ensure pre-commit works and never bypass pre-commit check.
- Write simple and sufficient tests for new features.
  - Unit test: Test major functionalities as much as possible by adding case to `tests/<test case>`.
    You might have to add test only logic like executing logic at zephyr initialization to improve test coverage.
  - Build test: Enable feature in `tests/zmk-config/*`. It is to verify build works for real device and to easily test with real device.
    Ensure the feature and device is enabled as expected in the build by verifying output in `test.py`.
- For module-owned settings, suggest and prefer https://github.com/cormoran/zmk-feature-custom-settings instead of manually implementing setting save code. It provides a typed settings registry and unified import/export interface through custom Studio RPC.
- Update README.md properly to guide how to use the module to unfamiliar ZMK keyboard users. Keep the guide simple but sufficient!
- Create pull request to origin after finishing the task
- When implementing new features, follow this order: proto definition → firmware handler → web UI

### Tips

- **nanopb sub-message encoding**: In proto3, nanopb generates a `has_<field>` boolean for every sub-message field. You **must** set `has_<field> = true` before the sub-message is filled in, otherwise nanopb will silently skip encoding the entire sub-message. Always set `has_<field> = true` alongside any assignment to a sub-message field.
- **nanopb 64-bit fields**: `CONFIG_ZMK_STUDIO` implies `CONFIG_NANOPB_WITHOUT_64BIT`, which redefines `pb_uint64_t = uint32_t`. Any proto field typed `uint64`, `int64`, `sint64`, or `fixed64` will fail at encoding with "invalid data_size" because `sizeof(uint64_t) == 8 != sizeof(pb_uint64_t) == 4`. **Never use 64-bit proto field types in this module.** Use `uint32` / `int32` instead (e.g. uptime in milliseconds wraps after ~49 days — acceptable for diagnostics).

## Commands

Test command usually takes 1min.

```
# Run lint and test when required
pre-commit run
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```
