# cormoran's ZMK Module Template for ZMK

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-module-template/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-module-template/actions/workflows/zmk-module.yml) [![Devcontainer](https://github.com/cormoran/zmk-module-template/actions/workflows/devcontainer.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-module-template/actions/workflows/devcontainer.yml)

This repository contains a template for a ZMK module, as it would most frequently be used.

It's extended from ZMK official template with applying [zmk-west-commands](https://github.com/cormoran/zmk-west-commands), adding test code template and coding agent supports.

## Usage

Read through the [ZMK Module Creation](https://zmk.dev/docs/development/module-creation) page for details on how to configure this template.

## More Info

For more info on modules, you can read through through the [Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html) and [ZMK's page on using modules](https://zmk.dev/docs/features/modules). [Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests) may also be of use.

## Module User Guide

1. Add dependency to your config/west.yml.

   ```yml
   manifest:
       remotes:
           ...
           - name: cormoran
           url-base: https://github.com/cormoran
       projects:
           ...
           - name: zmk-module-template
           remote: cormoran
           revision: main # or latest commit hash
           # import: true # if this module has other dependencies
           ...
   ```

2. Enable flag in your config/<shield>.conf

   ```conf
   CONFIG_YOUR_MODULE_NAME=y
   ```

3. Update your <keyboard>.keymap like .....

   ```
   / {
       ...
   }
   ```

## Module Development Guide

### Setup for running test

#### Option1: west workspace directory layout

Set west topdir as parent of repository root and download dependencies under `../`.
This layout is useful to reduce disk usage by sharing dependencies with other zephyr modules.
The build result is located in `../build`.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
# rm -r .west # if exists to reset workspace
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

#### Option2: isolated directory layout

Set west topdir as repository root and download dependencies under `./dependencies`.
This layout is useful if you don't want to share dependencies to other zephyr modules.
Dev container and github actions uses this layout.
The build result is located in `./build`.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

### Pre-commit

Every commit need to pass pre-commit verification. The verification contains formatting code and running tests.

```
pip install pre-commit
pre-commit install

# Run pre-commit manually
pre-commit run --all-files
# Run for git staged files
pre-commit run
```

### Running Test

```bash
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
```

### Coding agent on actions

Actions for github copilot, claude (experimental) and codex (not useful yet) are available.

- Mention `@copilot`
- Setup `OPENAI_API_KEY` secret and mention `@codex`
- Setup `ANTHROPIC_API_KEY` secret and mention `@claude`
