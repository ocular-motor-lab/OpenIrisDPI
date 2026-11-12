# OpenIrisDPI for OpenIris 2

This folder is the DPI plugin for **OpenIris 2**, the C++ rewrite of OpenIris: Ryan Ressmeyer's
dual Purkinje imaging, whose plugin for OpenIris is next to it in [`../OpenIrisDPI`](../OpenIrisDPI).
It tracks, per eye and per frame, the pupil, P1 (the corneal reflection) and P4 (the reflection from
the back of the lens) and gives the DPI signal P4 − P1, and turns it into gaze with a calibration on
targets. The method is Ryan's; the code is a new implementation for OpenIris 2, written from a
reading of his. [DPI-SPEC.md](DPI-SPEC.md) has the method, the settings and what differs from his
plugin. Not yet checked on real DPI images.

If you use it, please cite Ryan's paper:

> Ressmeyer, R. A., et al. "OpenIrisDPI: An open-source digital dual Purkinje image eye tracker for
> visual neuroscience." *Journal of Neuroscience Methods* (2026): 110693.

## What it gives

| | |
|---|---|
| **Steps** | `glints.p1` (P1), `pupil.hull` (the pupil, as the convex hull of its dark pixels), `dpi.p4` (P4 and P4 − P1) |
| **Calibration** | `gaze.dpi`: P1 and P4 to gaze angles, a regression on targets (one target from an average eye, or a grid) |
| **Outputs** | `dpi/p1`, `dpi/p4`, `dpi/p4_p1`, and the calibration's model `dpi/model/gaze` ([outputs/dpi](outputs/dpi)) |
| **Profiles** | *DPI rig* (two Teledyne cameras at 500 Hz), *Synthetic eyes, DPI* and *Rendered eyes, DPI* (no camera needed) ([profiles](profiles)) |

## Building it

It's built against a build of OpenIris 2, with the plugin kit that every OpenIris 2 build writes in
its folder (OpenIris 2's `docs/building.md` and PLUGINS-SPEC 22). The module only loads into an
OpenIris 2 built from the same version of its headers and with the same compiler: rebuild this one
whenever you update OpenIris 2.

### macOS

You need what OpenIris 2 needs (Xcode's command line tools, CMake 3.24 or later, Ninja), and
OpenIris 2 built, say in `~/Developer/OpenIris2/build`. Then:

```
cd ~/Developer/OpenIrisDPI/openiris2
cmake --preset macos -DOpenIris2_DIR=$HOME/Developer/OpenIris2/build
cmake --build build
ctest --test-dir build
cmake --install build
```

`cmake --install` copies the module, `openiris-dpi.dylib`, into your plugins folder,
`~/Library/Application Support/OpenIris2/plugins`. The next time OpenIris 2 starts, it's in
Settings · Plugins under *Added on this computer*, and its profiles are in the Profiles list.

### Windows

Not yet tried: OpenIris 2 hasn't been built on Windows with plugins outside it. The steps would be
the same, in a *Developer PowerShell for VS 2022*, against OpenIris 2 built on the same PC with the
same Visual Studio (x64, Release):

```
cd $HOME\Developer\OpenIrisDPI\openiris2
cmake --preset windows -DOpenIris2_DIR=$HOME\Developer\OpenIris2\build
cmake --build build --config Release
ctest --test-dir build -C Release
cmake --install build --config Release
```

The module, `openiris-dpi.dll`, goes into `%LOCALAPPDATA%\OpenIris2\plugins`.

### While working on it

Instead of installing, point OpenIris 2 at the build: `OPENIRIS_PLUGINS=<this folder>/build/plugins`.
The tests do that themselves: they link the plugin's code to test its pieces, and load its module
(and OpenIris 2's rendered eyes, from OpenIris 2's build) to run it in the tracker. The tests on
rendered eyes are built when OpenIris 2's build has the rendered plugin.

## Files

| | |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | the build: `find_package(OpenIris2)` and `openiris2_add_plugin(dpi …)` |
| `plugin.cpp` | the registration |
| `dpi.hpp`, `stages_dpi.cpp` | the algorithms and the three steps |
| `calibration_dpi.cpp` | `gaze.dpi` |
| `outputs/dpi/` | the definitions of the plugin's outputs |
| `profiles/` | its three profiles |
| `tests/` | tests on synthetic DPI images with known positions, on the synthetic eyes with P4, and on OpenIris 2's rendered eyes against their exact truth |
| `DPI-SPEC.md` | the design |

## License

GPL-3.0, as the rest of this repository ([LICENSE](../LICENSE)).
