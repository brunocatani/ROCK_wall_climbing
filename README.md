# ROCK Wall Climbing

A Fallout 4 VR addon for ROCK that lets you grab walls and pull yourself through the world with your hands.

## Features

- Climb with either hand or both, holding the same surface or separate surfaces.
- Move hand over hand and use your climbing momentum to launch yourself when you let go.
- Press your normal jump control near a reachable ledge to attempt a jump up. Your hold releases only when a suitable ledge and clearance are found.
- Follow supported moving surfaces while holding on.

Climbing yields to other supported grab interactions. Power armor frames and actors in power armor are excluded as climbing surfaces.

## Requirements and installation

- Fallout 4 VR and F4SEVR.
- ROCK and its requirements, including FRIK v78.2 or higher.

Install through your mod manager and launch the game through F4SEVR. The plugin belongs at `Data/F4SE/Plugins/ROCK_Wall_Climbing.dll`.

## How to climb

Touch a surface, hold the grab button, and move your hand opposite the direction you want to travel. Release to let go. Surfaces need usable collision to be grabbed.

## Settings

The active configuration is in your Documents folder:

```text
My Games/Fallout4VR/Mods_Config/ROCK_Wall_Climbing/ROCK_Wall_Climbing.ini
```

The mod uses compiled defaults when this file is absent. To customize movement response, smoothing, or launch strength, create the file with the sections and keys you want to change. See the [configuration reference](data/config/ROCK_Wall_Climbing_example.ini) for available settings. Reload your save after editing.

<details>
<summary>Building from source</summary>

Requires Windows x64, Visual Studio 2022 with the v143 C++ toolset, CMake 4.2 or newer, vcpkg, CommonLibF4VR, and ROCK's source headers. The project uses C++23.

Copy [CMakeUserPresets.json.template](CMakeUserPresets.json.template) to `CMakeUserPresets.json`. Configure your CommonLibF4VR path, `ROCK_SOURCE_INCLUDE_DIR`, and deployment destination; adjust the vcpkg path in your local presets if needed. The `custom-fast` preset copies the plugin to the mod folder you configure.

```powershell
cmake --preset custom-fast -B build-fast
cmake --build build-fast --config Release --target ROCK_Wall_Climbing -- /m:1 /p:CL_MPCount=2

cmake --preset custom-tests -B build-tests
cmake --build build-tests --config Release --target ROCKWallClimbingPolicyTests -- /m:1 /p:CL_MPCount=2
ctest --test-dir build-tests -C Release --output-on-failure -j 4
```

</details>

## Source and license

[Source code](https://github.com/brunocatani/ROCK_wall_climbing) · [GNU GPL v3 only (GPL-3.0-only)](LICENSE).

Built with the assistance of AI.
