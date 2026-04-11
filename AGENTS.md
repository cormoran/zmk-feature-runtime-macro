## Setup

`./.devcontainer/init.sh`

It initializes west with Option2 (download deps in ./dependences) explained in README.md.
Users might already initialized repo with Option1 (download deps in ../ and share with other modules).

## Initialization

This repo is created from template. Below template code should be changed when starting implementation.

- README.md
  - Remove Summary, "Usage" and "More Info" which is for template users. Replace to proper module description.
  - Update "Module User Guide" properly
  - Update CI status badge to link to your repo instead of template
- zephyr/module.yml: Change module name properly.

## Dev Rules

- Ensure pre-commit works and never bypass pre-commit check.
- Write simple and sufficient tests for new features.
  - Test functionality with unit test (add tests/<test case>/\* and run `python3 -m unittest`)
  - Test build works for real device by enabling new feature in `tests/zmk-config/*` and run `python3 -m unittest`
    Also ensure the feature is enabled in the build by verifying output in `test.py`.
- Update README.md properly to guide how to use the module to unfamiliar ZMK keyboard users.
