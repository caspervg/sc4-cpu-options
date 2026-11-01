# sc4-cpu-options

A DLL Plugin for SimCity 4 that configures the CPU core count and priority.

It has no settings and no configuration file: everything is automatic.

* **One core for the game, the others for the parallel render cull**: SimCity 4's own threads run on one core
(a performance core on a hybrid CPU), which avoids the game's multi-core crashes. Only the game's threads are
restricted: the parallel render cull's worker threads run on the other cores, off the game's physical core.
* **High priority**: the game runs at high CPU priority. A `-CPUPriority` command line argument still wins,
with these values: High, AboveNormal, Normal, BelowNormal, Idle, and Low (which the game applies itself).
* **Performance cores**: on a hybrid CPU (Intel 12th generation and later) the game's threads stay on the performance
cores. No effect on other CPUs.
* **Power throttling**: Windows 11 no longer slows the game down (EcoQoS) when it is not the foreground window.
* **Parallel render cull**: the search for what is visible in the 3D view is shared among the render thread and up
to 7 workers on the other cores (it needs at least 2 free logical processors besides the game's core). The sorting
and drawing stay on the game's render thread, unchanged.
* **Simulation tick budget**: the simulation may take at most 32 ms before the game draws the next frame,
instead of up to 66 ms, so scrolling and the interface stay smooth in a busy city.
* **Simulation early exit**: the game gives the simulation a fixed part of every frame (33, 50 or 66 ms by speed),
even when it has nothing left to do, which limits the frame rate to 30, 20 or 15 fps. The plugin ends that part
once the simulation has caught up, so the frame rate is limited only by the real work. Sim days pass at the
same rate as before. A simulation that cannot keep up gets its full time, as in the unmodified game.

The last four come from the [SCD3D11](https://github.com/caspervg/scd3d11) graphics driver. The parallel render
cull and the simulation tick budget patch the game's code and require SimCity 4 version 1.1.641; on other versions
they stay off.

For troubleshooting, these command line arguments turn parts off:

| Argument | Effect |
--------|--------
| `-ParallelCull:off` | No parallel render cull. |
| `-SimTickCap:off` | The game's own simulation budget of 33 to 66 ms. |
| `-SimTickCap:<ms>` | Another budget, 15 to 500 ms. |
| `-SimEarlyExit:off` | The game's own fixed simulation time per frame. |
| `-SimEarlyExit:<ms>` | After the simulation has caught up, its background (idle-time) work may run this much longer before the frame ends, 0 to 100 ms. Default 2. |
| `-SimDayWait` | Before ending the simulation's part of a frame early, wait for a sim day that is due before the next frame. Cheetah may then reach a few more sim days per second, at a lower frame rate. Default off. |
| `-SimDayWait:<ms>` | The same, for a sim day due within this many ms, 0 to 100, instead of the measured time between frames. |
| `-SimFrameCap:<fps>` | Spend what is left of frames faster than this on the simulation's background work, 10 to 1000 fps. Default off. |
| `-SimSliceProbe` | Log frame rate, sim days per second and simulation timing every 5 seconds. |
| `-CPUCount:<n>` | The game's own option: the game picks its cores, the plugin does not restrict its threads, and the parallel render cull's workers share the same cores. When the game is left with one core, the cull stays off. |

On a slower PC or graphics card, or to keep a laptop cooler, cap the frame rate with `-SimFrameCap:60` (or `30`).
The time a frame does not need then goes to the simulation's background work, as in the unmodified game, and the
graphics card renders fewer frames. With vertical sync on, the frame rate is already capped at the screen's refresh
rate. If a busy city's buildings seem to grow more slowly, raise `-SimEarlyExit` (for example to 10), or use
`-SimFrameCap`.

If SCD3D11 is also installed, the plugin loaded second finds the game's code already patched and leaves it alone,
so the patches are applied once.

This plugin is incompatible with 3rd-party launchers that set a CPU priority, it will override the CPU priority
setting they set when starting the game.
 
The plugin can be downloaded from the Releases tab: https://github.com/0xC0000054/sc4-cpu-options/releases

## System Requirements

* Windows 10 or later

The plugin may work on Windows 7 or later with the [Microsoft Visual C++ 2022 x86 Redistribute](https://aka.ms/vs/17/release/vc_redist.x86.exe)
installed, but I do not have the ability to test that.

## Installation

1. Close SimCity 4.
2. Remove `SC4SingleCPU.dll` and `SC4CPUPriority.dll` from the Plugins folder in the SimCity 4 installation directory, if present.
3. Remove `SC4CPUOptions.ini` from the Plugins folder, if present; the plugin no longer reads it.
4. Copy `SC4CPUOptions.dll` into the Plugins folder in the SimCity 4 installation directory.

## Troubleshooting

The plugin should write a `SC4CPUOptions.log` file in the same folder as the plugin.    
The log contains status information for the most recent run of the plugin.

# License

This project is licensed under the terms of the GNU Lesser General Public License, version 2.1 or later.    
See [LICENSE.txt](LICENSE.txt) for more information.

## 3rd party code

[gzcom-dll](https://github.com/nsgomez/gzcom-dll) Located in the vendor folder, MIT License.    
[SCD3D11](https://github.com/caspervg/scd3d11) - Original source for several CPU fixes, GNU Lesser General Public License, version 2.1 or later License.

The files that come from SCD3D11 are (`CpuScheduling`, `ParallelRenderCull`, `SimTickBudget` and `tests/CpuSchedulingTest.cpp`).

# Source Code

## Prerequisites

* Visual Studio 2022 or later, with the *Desktop development with C++* workload

The project builds with the toolset of whichever Visual Studio opens it, and needs no NuGet or vcpkg packages.

## Building the plugin

* Open the solution in the `src` folder
* Optionally, set `SC4PluginsDir` to your SimCity 4 Plugins folder, as an environment variable or in the project's
`UserMacros`, so each build is copied there. Without it the DLL stays in the build output folder.
* Build the solution

## Testing

`tests/CpuSchedulingTest.cpp` checks the performance-core selection. Build it as a 32-bit console program with
`src/CpuScheduling.cpp` and `src/Logger.cpp` in the Debug configuration, so the asserts are on; it returns 0 when every
check passes.

## Debugging the plugin

Visual Studio can be configured to launch SimCity 4 on the Debugging page of the project properties.
I configured the debugger to launch the game in a window with the following command line:    
`-intro:off -w -CustomResolution:enabled -r1920x1080x32`

You may need to adjust the resolution for your screen.
