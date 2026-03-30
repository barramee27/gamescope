# gamescope

Micro-compositor for gaming: the same role as the old `steamcompmgr`, with lower latency and a cleaner path from game frames to the display.

## About this repository

This fork extends [Valve’s gamescope](https://github.com/ValveSoftware/gamescope) with fixes for **NVIDIA proprietary drivers** on Linux, with emphasis on **RTX 40/50 (Blackwell)** and **driver 570+**, especially **hybrid laptops** (NVIDIA dGPU + integrated GPU running the desktop compositor).

**Branch with the NVIDIA work:** `feat/nvidia-blackwell-explicit-sync`

Upstream changes should eventually be proposed back to Valve; this README describes behavior on **this branch**.

### What the NVIDIA patches change

- **Nested Wayland + NVIDIA:** If `/dev/nvidiactl` exists and you run under a Wayland session with `--backend auto` (default), gamescope selects the **SDL** backend instead of the Wayland backend. The Wayland backend shares output via DMA-BUF to the parent compositor, which often **fails on hybrid NVIDIA setups** (invalid `wl_buffer` / broken pipe).
- **Vulkan:** Detects `VK_DRIVER_ID_NVIDIA_PROPRIETARY`, skips Mesa-only paths (e.g. certain WSI memory import), and applies safer defaults for flippable images and sync.
- **DRM / KMS:** Disables explicit sync by default on NVIDIA (implicit sync fallback); optionally enables an experimental **SYNC_FD** path via ConVar `drm_nvidia_explicit_sync_via_sync_fd`. Proactively disables `IN_FENCE_FD` where it causes atomic commit failures.
- **Wayland backend:** If you force `--backend wayland`, DMA-BUF modifiers are checked before `create_immed` to avoid fatal protocol errors; explicit sync is not advertised for NVIDIA clients the same way as on Mesa.
- **Preemptive upscale / timelines:** NVIDIA-specific semaphore bridging and audited error handling (no silent GPU sync skips, FD leaks fixed).

**Tested by the maintainer:** RTX 5050 Laptop GPU, driver 570, Pop!_OS 24.04 (Wayland), including real games via Steam/Proton.

**Not a guarantee** for every NVIDIA GPU, every driver version, or nouveau. Reports with `nvidia-smi`, distro, and desktop help narrow issues.

---

## Embedded vs nested

In an embedded session, gamescope can flip game frames with DRM/KMS with minimal copying. When nested on a normal desktop:

- The game runs in its own Xwayland sandbox; your desktop and the game do not stomp each other’s windows.
- You can expose a virtual resolution and refresh rate to the game and scale or letterbox the output (useful for ultrawide and multi-monitor setups).

---

## Requirements

- **Mesa (AMD / Intel):** as upstream: AMD Mesa 20.3+, Intel Mesa 21.2+. Older AMD GFX8 and below may need `R600_DEBUG=nodcc` until modifier support is solid.
- **NVIDIA proprietary:** DRM modesetting is still recommended (`nvidia-drm.modeset=1` where applicable). This fork targets recent proprietary stacks with Vulkan + DRM syncobj support; very old drivers may not match upstream gamescope’s baseline either.

Build **with SDL2** for nested use on NVIDIA (`sdl2_backend` enabled in Meson).

---

## Building

```sh
git clone https://github.com/barramee27/gamescope.git
cd gamescope
git checkout feat/nvidia-blackwell-explicit-sync
git submodule update --init --recursive

meson setup build/
ninja -C build/
./build/src/gamescope -- <game or command>
```

Install (optional):

```sh
meson install -C build/ --skip-subprojects
```

Use `meson configure build/` to toggle features (e.g. `pipewire`, `drm_backend`, `sdl2_backend`).

---

## NVIDIA: nested desktop usage

### Environment variables (typical hybrid laptop)

Prime offload to the NVIDIA GPU and force GLX to NVIDIA when launching GL apps under Xwayland:

```sh
export __NV_PRIME_RENDER_OFFLOAD=1
export __GLX_VENDOR_LIBRARY_NAME=nvidia
```

If the mouse or keyboard does not respond until you Alt+Tab or click the window, that is usually **X11/SDL focus**, not a failed launch. Click inside the gamescope window once after it appears, or Alt+Tab once.

### SDL on X11 (often helps input under Wayland session)

```sh
export SDL_VIDEODRIVER=x11
```

### Example: run gamescope then a game

```sh
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia \
SDL_VIDEODRIVER=x11 \
/path/to/gamescope/build/src/gamescope -W 1920 -H 1080 -f -- steam steam://rungameid/2231380
```

Use your real path to the `gamescope` binary (or install prefix).

### Steam per-game launch options

Keep existing options that you still need (Proton, MangoHud, etc.) and **always** end with `-- %command%`.

```sh
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia SDL_VIDEODRIVER=x11 /path/to/gamescope -W 1920 -H 1080 -f -- %command%
```

You do not need to “clear” launch options unless two wrappers conflict; merge env vars on one line when possible.

### Logs

Capture everything to a file:

```sh
... gamescope ... &> gamescope.log
```

You should see messages such as **NVIDIA proprietary driver detected** and, under Wayland, **using SDL backend for nested Wayland mode** when the auto-backend logic applies.

### Experimental explicit sync on NVIDIA (optional)

```sh
GAMESCOPE_CONVAR="drm_nvidia_explicit_sync_via_sync_fd 1"
```

Only for testing; default remains implicit sync on NVIDIA.

---

## Keyboard shortcuts

- **Super + F** — Toggle fullscreen  
- **Super + N** — Toggle nearest-neighbour filtering  
- **Super + U** — Toggle FSR upscaling  
- **Super + Y** — Toggle NIS upscaling  
- **Super + I** — Increase FSR sharpness  
- **Super + O** — Decrease FSR sharpness  
- **Super + S** — Screenshot (default `/tmp/gamescope_$DATE.png`)  
- **Super + G** — Toggle keyboard grab  

Run `gamescope --help` for the full list.

---

## Examples

```sh
# Integer-scale 720p content to 1440p
gamescope -h 720 -H 1440 -S integer -- %command%

# Cap a vsynced game at 30 FPS
gamescope -r 30 -- %command%

# 1080p game, pillarboxed fullscreen on 3440×1440
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```

---

## Common options

See `gamescope --help` for everything.

| Flag | Meaning |
|------|--------|
| `-W`, `-H` | Output (gamescope) resolution; ignored in embedded DRM mode. Default 1280×720. |
| `-w`, `-h` | Game-internal resolution; defaults follow `-W`/`-H`. |
| `-r` | FPS limit while focused. |
| `-o` | FPS limit while unfocused. |
| `-F fsr` / `-F nis` | FSR / NIS upscaling. |
| `-S integer` / `-S stretch` | Scaling mode. |
| `-b` | Borderless window. |
| `-f` | Fullscreen window. |
| `--backend` | `auto`, `sdl`, `wayland`, `drm`, `headless`, … |

---

## Reshade

Gamescope can load a subset of ReShade effects via `--reshade-effect` and `--reshade-technique-idx`. That adds latency (work on the general graphics/compute queue). For simple color transforms, prefer built-in LUT/CTM paths where available. See upstream docs and `gamescope --help`.

---

## Packaging status (upstream)

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg?exclude_unsupported=1)](https://repology.org/project/gamescope/versions)

Distro packages track **upstream** Valve gamescope; this fork must be built from source until changes are merged upstream.

---

## License and upstream

gamescope is developed by Valve and contributors. This fork inherits the same license as upstream; see repository files for details. For the canonical project and issue tracker, see [ValveSoftware/gamescope](https://github.com/ValveSoftware/gamescope).
