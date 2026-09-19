# Decomp2Gecko

Turn C source edits in the [Melee decomp](https://github.com/doldecomp/melee) into Gecko codes that run against the vanilla DOL.

Edit C and hit Generate. No hand-written ASM. Both players use unmodified ISOs.

<img width="1791" height="937" alt="Screenshot_20260918_234129" src="https://github.com/user-attachments/assets/2d771cbc-e854-4817-8ee5-bcebfd97d93f" />

## Download

Pre-built binaries for Windows, macOS, and Linux are available on the [Releases](https://github.com/johnqherman/Decomp2Gecko/releases) page.

## How it works

The Melee decomp build already produces MWCC object files for every translation unit. Those objects are compared against the vanilla DOL to determine what actually changed.

Unchanged code stays where it is. Changed code and data is rewritten in place where it fits. A function that grew stays put too (new instructions go to free memory above `__ArenaHi`). Data that already exists in the vanilla DOL is pointed at instead of written.

The output is a set of Gecko write codes (`04` and `06`) wrapped in a one-shot guard so the patch is applied once per boot.

Slippi's existing hooks are checked automatically, and relocations that would collide with them are refused.

## Building

You'll need:

* A C++17 compiler
* CMake 3.21+
* Ninja
* Qt 6 Widgets

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/src/gui/Decomp2Gecko-gui
```

## Using the app

Point the app at an unmodified NTSC 1.02 ISO and your Melee decomp checkout.

On first run, the app extracts `main.dol` from the ISO to use as the vanilla baseline.

Hit **Generate** to:

1. Build the decomp
2. Relink modified code
3. Verify every changed function against the build's `main.elf`
4. Generate Gecko codes

The mod name is filled in automatically from the current git branch.

You can then save the generated codes as an `.ini`, or install them directly into Slippi Dolphin's GameSettings. Before installing anything, the app shows exactly what it will change and backs up the existing file.

## Limits

Only the DOL is patched. Models, textures, audio, and other files stored on the disc can't be changed with Gecko codes. Asset mods still require a modified ISO or other approach.

A few other constraints are worth knowing:

* Small-data sections (`.sdata`, `.sbss`, `.sdata2`, `.sbss2`) can't grow. New translation units need `-sdata 0`.
* Functions already hooked by Slippi with C2 codes can't be relocated. These conflicts are reported at generation time.
* Gecko codes take effect from the first frame, so code that only runs during the boot path can't be patched in time.
* Generated codes are appended to Slippi's existing ~7k lines of Gecko codes, so total code size still matters.
* Relocated `.bss` is zero-filled.

## Tests

Run the unit and integration tests with:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests are self-contained and run entirely from the repository.

The main integration test, `test_layout.cpp`, creates a synthetic decomp checkout from scratch and runs it through the full linker pipeline. A clean tree must produce no changes, while a modified tree must produce an exact, independently derived Gecko code list.

Generate performs another check at runtime. It replays the emitted Gecko codes against a vanilla DOL image and verifies that the patched result matches what was expected.
