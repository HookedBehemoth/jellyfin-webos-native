# Native applications on webOS TV (LG, `10.10.8.63`)

Findings from investigating how to run native code — and specifically Vulkan —
on an LG webOS TV. Everything here was verified on the device unless explicitly
marked as unverified.

| doc | contents |
|---|---|
| [device.md](device.md) | ABI, toolchain, build and deploy |
| [display.md](display.md) | How to get pixels on screen; the 1080p60 graphics plane vs the 4K120 video planes |
| [opengl.md](opengl.md) | OpenGL ES 3.2 capabilities, GPU timing, and the Slang -> GLSL ES pipeline |
| [fonts.md](fonts.md) | Installed TV fonts and the native UI font selection order |
| [vulkan.md](vulkan.md) | The Mali ICD, the missing loader, the missing WSI, and how to initialise anyway |
| [home.md](home.md) | **Home**: the launcher, chosen through Home Chooser, what it asks of it, the Luna calls it makes, what is unverified |
| [jellyfin.md](jellyfin.md) | **The Jellyfin client**: storage and artwork caching, the libpng-not-libjpeg decision, and playback — Starfish for video, ALSA for audio |
| [ndl.md](ndl.md) | **NDL: hardware video decode on the video plane** — interface, the four traps, punch-through |
| [multimedia.md](multimedia.md) | Hardware video decode: NDL_directmedia, GStreamer, device nodes |
| [codecs.md](codecs.md) | Full codec support: hardware limits, containers, which API to use |
| [packaging.md](packaging.md) | .ipk format, installing, and the reserved-namespace trap |
| [input.md](input.md) | Input devices and options; forwarding the PC cursor as a remote |
| [network.md](network.md) | Interfaces, link speeds, listening services |
| [device-codec-capability.json](device-codec-capability.json) | Raw LG codec capability table pulled from the TV |
| [gstreamer-codec-elements.txt](gstreamer-codec-elements.txt) | Raw inventory: 219 GStreamer codec elements + caps |
| [vulkan-extensions.txt](vulkan-extensions.txt) | Raw `vkinfo` dump: 9 instance + 102 device extensions |
| [ndl-symbols.txt](ndl-symbols.txt) | Raw export lists of the four NDL libraries |
| [opengl-capabilities.txt](opengl-capabilities.txt) | Raw `glinfo` dump: limits + 28 EGL and 101 GL extensions |

## The three facts that shape everything

1. **Userland is 32-bit ARM** (`arm-linux-gnueabi`), on a 64-bit kernel.
   `uname -m` says `aarch64` and is misleading. The float ABI is soft *for
   argument passing only* — the FPU is real and fast, and `-mfloat-abi=softfp`
   gets both. A toolchain that cannot express that costs 10.5x. See
   [device.md](device.md).
2. **The framebuffer cannot be written directly.** Scanout is AFBC-compressed and
   owned by `surface-manager`. Wayland is the only route to the screen — and
   that route is a **1080p60 graphics plane**, set per model by configd. 4K120
   belongs to the separate video planes and the hardware decoder, which is how
   Moonlight-style homebrew claims it. See [display.md](display.md).
3. **Vulkan works but cannot present.** The driver is a complete Vulkan 1.3.260
   ICD, but ships no `VK_KHR_wayland_surface` — so a normal swapchain cannot
   reach the display. Present via `zwp_linux_dmabuf_v1` instead, or use
   **OpenGL ES 3.2**, which has a working EGL/Wayland binding — see
   [opengl.md](opengl.md).

## Status

The repository now holds three C programs, built with CMake against the openlgtv
buildroot NDK:

- `jellyfin` — **a real Jellyfin client**: discovery, sign-in, home rows, a
  virtual library grid with server artwork, shows down to episodes, and
  **playback** — FFmpeg demuxes, Starfish decodes video onto the TV's own plane,
  and the decoded audio goes to ALSA. See [jellyfin.md](jellyfin.md).
- `xmb` — a full-screen shader with CPU/GPU frame times
- `gltri` — **3000 instanced rotating triangles at 60 fps**, same overlay

```sh
cmake --preset webos
cmake --build build
cmake --build build --target jellyfin-ipk      # -> build/dist/*.ipk
cmake --build build --target jellyfin-install  # honours ARES_DEVICE
```

Everything else in these documents was learned from probes that have since
served their purpose and been removed — `fbflash`, `wlinfo`, `wlbox`, `vkinfo`,
`fptest`, `inputlog`, `glinfo`, `ndlplay`, `uidemo`. They are referred to by
name below because the findings are theirs; the code is in the git history.

Not yet built:

- **the Vulkan triangle** — design in [vulkan.md](vulkan.md#getting-a-triangle-on-screen)

(The PC-cursor-as-remote idea was dropped — see [input.md](input.md).)
