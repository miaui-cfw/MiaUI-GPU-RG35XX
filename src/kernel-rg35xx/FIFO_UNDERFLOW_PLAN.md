# `fifo underflow 0x1` on the RG35XX: investigation and work plan

Status 2026-11-19: phase 0 done, phase 1 in progress (see "Progress" at the end). Nothing patched. Device facts below were read from the console (read-only) on that
date. The kernel tree cited is Actions' public 3.10.y tree (LeMaker/linux-actions, branch linux-3.10.y), fetched
into a scratch folder; it is not yet in the repo (see 2.4).

Tags: **[P]** proven by measurement, **[S]** read from source or registers, **[H]** hypothesis, still to test.

---

## 1. Evidence and hypothesis audit

### 1.1 What the symptom is

- **[P]** The visible glitch (the whole picture shifted sideways for one refresh) matches `fifo underflow 0x1` in
  dmesg one to one (AITD2 intro, user video vs kernel log, 2026-11-15).
- **[S]** The message is printed by `de_path_fcr_set()` (drivers/video/owl/dss/de_atm7059.c). That function runs on
  every flip: it rewrites each layer's critical-FIFO settings, sets the path's FCR ("go") bit, then reads
  `DE_OUTPUT_STAT`. A non-zero value is printed and cleared. So the flag is **sticky**: it says the FIFO ran dry
  *somewhere in the frame before this flip*. The printk time is the flip time, not the underflow time.
- **[P]** Underflow timestamps are exact multiples of the 60.46 Hz refresh, and always at the same phase of the
  game loop (≈12 ms after the present). That fits "reported at the next flip"; it does not locate the cause.
- **[S]** Every flip goes through `fb_set_var` → `owlfb_set_par` → `owlfb_apply_changes` → manager apply →
  `de_path_fcr_set`. The PVR display-class driver (OMAPLFB, `omaplfb_linux.c`) calls `fb_set_var` whenever the
  flip's Y offset exceeds `yres_virtual`. fb0 reports `virtual_size 640,960` (2 buffers), but the PVR uses 3
  (pan 0/480/960), which is why `set_par` used to print on every flip.
- **[S]** `OMAPLFBWaitForVSync()` is a stub that returns true at once. The kernel never waits for vsync; the flip
  lands whenever the GPU finishes. Only the userspace SDL (mali-fbdev) vsync wait before `eglSwapBuffers` keeps
  flips near the blanking.

### 1.2 Proven so far

| # | Fact | Source |
|---|------|--------|
| P1 | AITD's full-screen 320x200 sprite layer, converted, uploaded and drawn every frame, raised the rate (12-18 per 100 s). Uploading only on change cut it, but not to zero. | AITD bisect, 2026-11-15 |
| P2 | AITD drawing the scene into a 640x400 RGB565 + D24S8 FBO and blitting it to the window: 8, 1, 4 per 60 s. Drawing straight into the window: far fewer (2 in 6 min at a verified 60.5 fps). | AITD A/B, same build |
| P3 | AITD with **no GL drawing at all** (clear + swap only) still logged ~2 per 100 s: CPU-side traffic alone can trigger it. | AITD `NODRAW` run |
| P4 | Short runs logged 0: sm64coopdx (120 s title), OpenTyrian (90 s), Sonic Mania (90 s, its intro uploads a 1024x512 video frame every frame). These runs are short and show light scenes: weak evidence, re-measure (phase 1). | read-only comparison runs |
| P5 | Not the cause: window pixel format (565 vs 8888), SD/CD streaming, AdLib vs CD audio, IRQs on CPU0 (moved SGX/USB/DMA/SD/I2C to CPU1: no change), CPU frequency (userspace governor, fixed), temperature (51 °C). | AITD runs + user's PSX runs |
| P6 | PSX (PCSX-ReARMed r26 via MinArch, Dave Mirra BMX carnival level): emulator output geometry never changes; the rate tracks workload (clock 80: 1.9/min, clock 57: 1.4/min, threaded rendering off: 0.6/min). | user's logs |
| P7 | fb0 at 16 bpp breaks the PVR output (it keeps rendering 32 bpp): a yellow, zoomed picture. | user |

### 1.3 Registers read (read-only)

| Register | Value | Meaning |
|---|---|---|
| `DE_IF_CON` | `0x001F0000` | max outstanding reads: already at maximum (`de_special_init`) |
| NoC `0xB0500108` | `0x0F` | DE priority on the NoC: already at maximum (also written on resume) |
| `OVL0_CRITICAL_CFG` | `0x10` | critical-FIFO threshold; rewritten to `0x10` on **every flip** by `de_path_fcr_set` |
| DMM `0xB0290000` `SCH_CTRL` | `0x48000804` | DDR scheduler control (fields undocumented) |
| DMM `+0x04` CPU/BISP | `0x0818F808` | CPU port settings |
| DMM `+0x0C` ETHERNET/DE | `0x0000F801` | DE port settings |
| DMM `+0x10` DE_CRITICAL/USB2 | `0xF8010000` | DE "critical" port settings |
| DMM `+0x18` GPU3D/DAP | `0x00000000` | **GPU port: all fields 0 (default)** |
| DMM `+0x44..0x5C` monitor + PM counters | `0` | performance monitor present but idle: a per-master bandwidth meter we can use |
| CMU `DDRPLL` `0xB0160008` | `0x327` | DDR PLL enabled, freq field 0x27 (tree default 0x19); MHz formula to be confirmed |
| GPU | 400 MHz, dvfs `enable=0` | SGX clock fixed |

### 1.4 Working model

The display engine (DE) fetches only ~74 MB/s (640x480x4 at 60.46 Hz), a small share of DDR bandwidth. So the
average bandwidth is not the problem. The FIFO runs dry when **one fetch waits too long** behind other masters'
bursts (SGX tile stores, CPU NEON streaming, PSX VRAM writes). That makes it a latency and arbitration problem,
which explains why lighter workloads lower the rate without ever reaching zero.

### 1.5 Open hypotheses (each has a test in section 4)

| ID | Hypothesis | What would prove or kill it |
|---|---|---|
| H1 | The DE's critical threshold (`0x10`) is too low: priority escalation comes too late. | Raise the threshold (kernel patch, phase 3): the rate must fall under the same stress. |
| H2 | The DMM schedules the GPU port (and the CPU port) without limits, so SGX bursts starve the DE. | DMM port settings A/B (phase 3) and DMM counters showing bandwidth per master during an underflow (phase 2). |
| H3 | The per-flip `fb_set_var`/`set_par` path (a full overlay reconfiguration, issued wherever the scan is) causes or worsens it. Plain `pan_display` would be lighter. | Make fb0 virtual 640x1440 so the flips go through `pan_display` (userspace, `fbset`); compare rates. |
| H4 | GPU work that stores and reloads big render targets (FBO + D24S8, MSAA, resolves) makes the worst bursts. | Stress test modes FBO vs window, depth discard vs not (phase 1). |
| H5 | The CPU alone (memcpy/NEON streaming on 2-4 cores) can trigger it. | Stress test CPU-only modes (P3 says yes, at a low rate). |
| H6 | The DDR clock or timings are lower than the board allows. | Decode `DDRPLL`; compare with the vendor default and Batocera/stock firmware. |
| H7 | Flip timing matters: a flip landing mid-scan (stub vsync wait) reprograms the DE while it fetches. | Log the scan position at flip time (DE line counter register, if one exists) against underflows. |

---

## 2. Source, kernel and blob inventory

### 2.1 On the device (read 2026-11-19)

- Kernel `3.10.37 #40 SMP PREEMPT Wed Dec 13 01:46:08 UTC 2023`, cmdline `androidboot.dvfslevel=0x705x
  allenS500=1`. **Full `/proc/kallsyms`** (75k symbols, statics included: `de_path_fcr_set` = `c0331ed8`,
  `de_special_init` = `c0331df4`, `owlfb_set_par` = `c0343560`, `owlfb_pan_display` = `c034349c`).
- `/dev/mem` is not restricted (`devmem` reads and writes work, kernel text readable: used by the set_par patch).
- No `/proc/config.gz`, no kprobes, no ftrace symbols; debugfs is in the kernel; `perf_event_paranoid` exists.
- **Modules load** (vermagic `3.10.37 SMP preempt mod_unload ARMv7 p2v8`). Loaded: `pvrsrvkm`, `drm`,
  `gpio_keys_polled`, `atc260x_cap_gauge`.
- DE driver: **built into the kernel**. Exported for modules: `owl_de_register_isr`, `owl_de_atm7059`, `fb_set_var`.
- GPU kernel driver: `pvrsrvkm.ko` (module), DDK `SGX_DDK_Linux_XOrg sgxddk MAIN@3625561`, SGX 544 rev 115.
- GPU userspace (**closed blobs**, 1.14.3625561): `libGLESv2`, `libEGL`, `libIMGegl`, `libsrv_um`, `libsrv_init`,
  `libusc`, `libglslcompiler`, `libpvr2d`, the WSEGL backends (`FLIP`, `LINUXFB`, `BLIT`, `FRONT`, `DRI`) and
  `libPVROCL`. No `/etc/powervr.ini`.
- fb0: 32 bpp, `virtual_size 640,960`, `size` 12,288,000 bytes (room for 10 buffers), `phys_addr 0x0be0c000`.

### 2.2 Local, under `/home/almacen/Desarrollo/anbernic/koriki`

- `boot/SD-boot/uImage` (+ `kernel.dtb`, `*-gpu.dtb`, `k-2100-stable.dts`, `ramdisk.img`, `FWORG/modules/pvrsrvkm.ko`).
- `libs/kernel.ko/lib/modules/3.10.37/` (module tree incl. `drivers/gpu/pvr/eurasia_km_linux/`), `libs/local-gl/`
  (Imagination test tools: `sgx_flip_test`, `sgx_render_flip_test`, `sgx_blit_test`, `services_test`,
  `pvrsrvctl`, `xeglinfo`...).
- `rg35xx-miau-toolchain/workspace/src/tools/patch_uimage_setpar.py` and `silence_setpar.py`: a **proven,
  emulator-verified way to patch the kernel image on disk** (gzip repack, U-Boot CRCs) and to check the running
  kernel text. It is the template for any DE patch.
- `workspace/src/platform/de_atm7059.h`: the DE register map (MinUI's copy).
- **No kernel source and no kernel `.config` locally.**

### 2.3 Public source that matches

Actions' tree (LeMaker/linux-actions, linux-3.10.y) has:
- `drivers/video/owl/dss/` (DE: `de_atm7059.c`, `manager.c`, `de.c`) and `drivers/video/owl/fb/` (owlfb);
- `drivers/gpu/pvr/eurasia_km/` at **the same DDK build 3625561** as the device, including the OMAPLFB display
  class (`dc_omapfb3_linux`);
- `arch/arm/mach-owl/` (register map with DMM/NIC/CMU, clock tree, PM).

Not proven yet: that the device's built-in kernel was compiled from this tree. To check: disassemble
`de_path_fcr_set` from kernel text (`/dev/mem` or the decompressed `uImage`) and compare it with the source,
constant by constant (the `0x10` store, register offsets). Same for `pvrsrvkm.ko` (symbol names, strings,
`owl_gpu_*` hooks present in the blob).

### 2.4 What each layer allows

| Layer | Have | Observe | Change |
|---|---|---|---|
| DE driver (built-in) | source (to verify), symbols | `devmem` reads of DE/DMM/CMU; a module registering `owl_de_register_isr` | on-disk uImage patch of `de_path_fcr_set` constants (same method as the set_par patch); runtime text patch for tests only |
| DMM / NoC | register map, no field docs | DMM performance counters (`PM_CTRL0/1`, `PC0-3`) | `devmem` writes (test only), later a boot-time init |
| PVR kernel driver (module) | **matching source** | rebuild with timing instrumentation (flip time, kick count, IRQ load) | rebuilt module: real `WaitForVSync`, flip via `pan_display`, flip in the vsync IRQ |
| PVR userspace | closed blobs | `LD_PRELOAD` shim on `eglSwapBuffers`/`gl*` (timing, call counts, upload sizes); `strace` of the ioctls | wrap only (no binary patches unless something proves necessary) |
| Apps (MinArch, ports) | source | our own counters | upload/draw changes per app (last resort, not the fix) |

---

## 3. Deterministic stress test (`fifostress`)

**Goal:** one static binary that triggers `fifo underflow 0x1` on demand and reports a rate. It must run unchanged
on any MiaUI version, and testers must be able to run it.

### 3.1 Design

- **Static, no SDL:** C, built in the koriki toolchain, linked statically except the GPU blobs. `libEGL` and
  `libGLESv2` are opened with `dlopen` at run time, so one binary fits any firmware. EGL on `fbdev` directly
  (`eglGetDisplay(EGL_DEFAULT_DISPLAY)` + native window `NULL`, which is how the PVR `FLIPWSEGL` backend works),
  so it does not depend on SDL's vsync wait. Optional mode `--sdl` (dlopen SDL2) reproduces the real games'
  swap order.
- **Measures itself:** reads the kernel log through `klogctl(SYSLOG_ACTION_READ_ALL)` at start and at the end
  (never a live `/proc/kmsg` reader: those have wedged the vsync before). It counts underflows inside its own
  time window, measures fps and frame time, and prints one result line (and a JSON line with `--json`).
- **No SD writes:** output to stdout only.
- **Safety:** runs at most N seconds (default 60), restores the CPU clock it changed, refuses to start if another
  GL client holds the display, and leaves MinUI parking to the caller (it says so).
- **Modes**, each a fixed and repeatable load, combinable:

| Mode | Load | Tests |
|---|---|---|
| `idle` | clear + swap | baseline, must be 0 |
| `fill N` | N full-screen opaque quads | GPU fill traffic |
| `fbo` | scene in a 640x400 RGB565 + D24S8 FBO, blit to the window | H4 (AITD's old path) |
| `fbo-discard` | `fbo` + `glDiscardFramebufferEXT` of depth/stencil | H4 |
| `upload WxH` | streaming texture upload every frame (3-texture rotation), then drawn | MinArch / PCSX path |
| `upload-same` | upload then sample the same texture in the same frame | AITD sprite pattern (P1) |
| `cpu K MB` | K MB memcpy/NEON stream per frame on 1-3 threads | H5 |
| `late` | flip pushed late into the scan (busy GPU before the swap) | H7 |
| `mix` | the measured AITD and PCSX profiles combined | the realistic case |

- **Tunable to the edge:** a `--ramp` option raises one load until the first underflow and reports that load. The
  threshold load is a far less noisy number than a rate. "Underflows per minute at fixed load" stays as the
  secondary metric.

### 3.2 Making it deterministic

1. Calibrate `mix` until it gives ≥10 underflows per minute on the current firmware. A tester then sees a clear
   number in 60 s, and a fix shows as a clear drop.
2. Run each configuration at least 3 times and report the median and range. AITD taught that two 100 s runs can
   differ by 10x.
3. Every result line carries fps, so a "fix" that only slows the frame rate (P6's threaded-off case, AITD's 6.7 fps
   build) is visible.

### 3.3 Distribution

A folder pak with the binary, a README and a one-line runner. The runner parks nothing and writes nothing: the
tester stops MinUI by the documented method. Before any build goes to testers, the README lists the expected
baseline (for example "stock MiaUI: `mix` 12-20 per min").

---

## 4. Phases

Each phase ends with numbers in this file and in `RG35XX_PowerVR_SGX_Dev_Notes.md`. No phase changes the boot
image or MiaUI until phase 5.

### Phase 0: Ground truth (½ day)

1. Verify that the device kernel matches the public tree: `de_path_fcr_set`, `de_special_init`, `owlfb_set_par`
   disassembly vs source; `pvrsrvkm.ko` vs DDK 3625561 (strings, exported symbols).
2. Get the matching `.config` (reconstruct it from the running kernel: `modules.builtin`, symbols present, and
   the tree's `owl_*_defconfig`), so modules build with the right vermagic.
3. Decode the `DDRPLL` frequency and the DMM fields (vendor tree, any S500/ATM7059 datasheet or U-Boot source).
4. Put the kernel tree and notes in the repo (submodule or pinned tarball + checksum).

*Exit:* a confirmed source match (or a list of differences), and the DDR MHz.

### Phase 1: Stress test (1-2 days)

1. Build `fifostress` with the modes above; check `idle` = 0 over 10 min.
2. Calibrate `mix` to ≥10 per min. Run every single mode alone to rank the contributors (H4, H5).
3. Re-measure the "clean" ports (P4) for 5 min each in heavy scenes, and MinArch+PCSX with the user's settings
   (any heavy PS1 3D game the user has) to check that the stress test reproduces the real case.

*Exit:* a reproducible baseline table (mode → median underflows per min, fps).

### Phase 2: Observation (2-3 days)

1. **DMM performance counters:** work out `PM_CTRL0/1` (writes to monitor registers only), and log bandwidth per
   master (CPU, GPU, DE) in 1 ms windows around underflows.
2. **Instrumented `pvrsrvkm.ko`** rebuilt from the matching source (test install in `/tmp`, `insmod` only after
   MinUI is parked; never replace the file on the card in this phase): per-flip timestamp, flip path
   (`set_var` vs `pan`), the time since the last vblank, SGX kick count per frame.
3. **Observation module** on `owl_de_register_isr`: vblank timestamps and `DE_OUTPUT_STAT` read in the IRQ, so
   the underflow is placed within one refresh rather than at the next flip.
4. **`LD_PRELOAD` shim** for the blobs (eglSwapBuffers / glTexSubImage2D / glDraw* / glBindFramebuffer
   counters), used with `fifostress`, AITD and MinArch.
5. H3 quick test: `fbset -vyres 1440` (all 3 PVR buffers inside the virtual size, so flips go through
   `pan_display`), stress test A/B. A runtime setting, undone by a reboot or `fbset -vyres 960`.

*Exit:* for each underflow, which masters were active and where the flip landed. H2, H3, H5 and H7 confirmed or
dropped.

### Phase 3: Mitigation experiments, runtime only (2-3 days)

All at run time (devmem, a test module, or the rebuilt PVR module loaded from `/tmp`), each one measured with the
stress test and undone at reboot:

| Experiment | Changes | For |
|---|---|---|
| E1 | `OVL_CRITICAL_CFG` 0x10 → 0x20/0x40/0x80, kept by a test module that re-writes it after `de_path_fcr_set` (or a runtime patch of the immediate in kernel text, test only) | H1 |
| E2 | DMM GPU port (`+0x18`) and CPU port (`+0x04`) settings, DE port weights, `DMM_AXI_PRIORITY` | H2 |
| E3 | Flips through `pan_display` only (`fbset`), and/or a real vsync wait in OMAPLFB | H3, H7 |
| E4 | GPU clock 400 → 300 MHz (dvfs sysfs), as a measurement only | H2 / H4 |
| E5 | DDR PLL one step up, only if phase 0 shows headroom; thermal and stability checked separately | H6 |

*Exit:* the smallest change that brings `mix` to ~0 at unchanged fps. Its cost must also be measured (fps of
AITD, MinArch PCSX, Sonic Mania; idle power).

### Phase 4: Application-side hygiene (in parallel, low risk)

Independent of the kernel result, and kept even if the kernel fix works:
- MinArch: count underflows on the `--gl` FBO path (the minarch future-work note); keep software cores on the
  direct streaming-texture path (already the case).
- Ports: draw into the window, upload only changed rectangles, discard depth/stencil (the AITD rules, already in
  the dev notes).
- PCSX: measure whether the GPU thread's VRAM writes (NEON) dominate (H5) before touching the core.

### Phase 5: Productising the fix (1-2 days + tester round)

1. Pick the winning change from phase 3 and ship it the least invasive way, in this order of preference:
   - a boot-time `devmem`/init setting (if DMM/NoC registers are enough and nothing rewrites them);
   - a rebuilt `pvrsrvkm.ko` (only if the flip path is the fix: it is a module, so it is replaceable and has a
     backup);
   - an on-disk uImage patch of `de_path_fcr_set` (same proven tool chain as the set_par patch: verify the
     surrounding words, write one aligned constant, recompute the CRCs, Unicorn boot check, keep `uImage.bak`).
2. Validate: `fifostress mix` (3 × 10 min), AITD intro, MinArch PCSX heavy scene, MinUI menus, faux sleep +
   wake, a full reboot cycle, 30 min of play at 1.2 GHz; dmesg clean apart from the counted lines.
3. Tester round with the stress-test pak: the before and after numbers on their own consoles.
4. Release path: the updater never touches p1 (the boot partition), so a uImage change needs the same manual
   step as the set_par patch. A userspace or module fix goes through `Update.pak`. Decide with the user.

### Rules for every phase

- Read-only first; every register write is a test, undone by a reboot, and logged in this file.
- No writes to the SD card from test tools; fixtures in `/tmp`.
- Every rate is reported with fps and the run length; median of ≥3 runs.
- Nothing lands on the card's boot partition before phase 5, and only with the user's explicit go-ahead.

---

## Progress

### Phase 0 (done, 2026-11-19)

- **Kernel = public tree.** `de_path_fcr_set` in the device kernel (from `boot/SD-boot/uImage`, a6da315d) matches
  LeMaker/linux-actions commit `7f697bae` instruction for instruction: register offsets, the `0x10` critical stores
  at VA `c0331fa8` (path 1) and `c03320bc` (path 2), FCR bit 29, `DE_OUTPUT_STAT` 0x100c. The tree is 3.10.100, the
  device 3.10.37: same driver code. `workspace/external/linux-actions/fetch.sh` downloads the pinned tree and checks
  the key files' SHA-256.
- **GPU module:** the device's `pvrsrvkm.ko` (8d33b1ee, built with Linaro GCC 4.9, symbols kept, no debug info,
  `struct module` 0x168 bytes, no modversions) contains OMAPLFB. `OMAPLFBFlip` always calls `fb_set_var` (the tree's
  config sets `PVR_OMAPLFB_DONT_USE_FB_PAN_DISPLAY`): **the planned `fbset` test cannot change the flip path.**
  The device module also has `owl_mmu_import_buffer_on_dc`, which the public PVR source lacks: it is a modified
  build of that source, not identical.
- **DDR:** `DDRPLL` = 39 x 12 MHz = **468 MHz** (tree default 300, max 804).
- **Tracing works without custom modules.** The kernel has ftrace with event tracing (debugfs, `trace_marker`):
  `owlfb-trace-events/owlfb_vsync` and `irq_handler_entry/exit`. Raw ring-buffer pages are copied off the device in
  seconds and decoded on the PC (`trace_decode.py`). Formatting the trace on the device took minutes: never do it.
- **Vsync, from source + trace:** SDL enables hardware vsync at start (`OWLFB_VSYNC_EVENT_EN`, ioctl 67), then waits
  with `OWLFB_WAITFORVSYNC` (57) every frame. The DE "pre-line" interrupt handler **busy-waits with IRQs off** (up
  to 500 µs, measured 380 µs median per frame on CPU 0) until vertical blanking, then wakes the waiter. Without
  ioctl 67 the wait returns at once.

### Phase 1 (in progress)

Tools in `workspace/src/fifostress/`: `fifostress` (stress test), `glshim.so` (GL call recorder, LD_PRELOAD),
`trace-capture.sh` (raw ftrace capture), `trace_decode.py` (decoder), `park-run.sh`, `run-aitd-old.sh`,
`run-aitd-cur.sh`.

Results:
- **No synthetic load triggers it** (all 0 underflows, 15-200 s each, with and without hardware vsync): GPU texture
  reads to saturation (4 fps), CPU copies flat out on 3 cores, up to 11 render-target passes per frame, up to 11
  full-screen uploads per frame (RGB565 or RGBA5551), AITD's frame structure (FBO + mid-pass 5551 upload + draws),
  silent ALSA audio, a flip at any point of the scan (0-14 ms), and combinations.
- **Reliable real repro: the pre-fix AITD build** (`rg35xx/build/push/Tatou`, 2026-11-14) on the AITD2 intro:
  1, 4, 4, 9 underflows per 50-60 s, clustered at ~29-45 s (gate, lion, run to the house). The current pak build
  forced onto the same path (`AITD_FBO=1 AITD_SPRITEMODE=9 AITD_NODISCARD=1`, also with `AITD_NOFINISH`,
  `AITD_CPU0`, `AITD_EAGER_BG`) gives 0-1.
- **Not the cause:** memory fragmentation / CMA exhaustion (4 and 4 underflows with memory clean or fully cached),
  uptime (first underflow 44 s into a run at 41 min uptime, after 20 min of clean maximum stress).
- **Kernel view of the old build (50 s, 9 underflows):** vsync steady (16.52-16.56 ms); no SD/USB/DMA bursts
  around underflows; GPU interrupt pattern of bad frames the same as clean ones. The underflow printk always lands
  3.6-3.9 ms after the pre-line interrupt: the flip, because the old build has the GPU render after the swap call.
- **GL view of the old build:** frames with the full-screen 320x200 RGBA5551 sprite upload have a **~10x higher**
  underflow rate (1.8% vs 0.18% per frame). The upload call takes ~1.06 ms of CPU for 128 KB (the driver twiddles
  on the CPU into GPU memory).

Next: a faithful replay of the old build's gate frames (shaders, textures, vertex data, state: an apitrace-like
capture of ~2 s through the shim), replayed in a loop by fifostress; then remove calls one by one until the
underflows stop. A second real repro (the PSX game) would cross-check that the trigger is the same.

### Phase 1 outcome (2026-11-19)

- The full GL record/replay (`gltrace.so` + `fifostress --replay`, with per-call timing, silent audio, and CHD
  decompression via libchdr) draws the gate scene pixel-exact at 60 fps, but **never underflows** (0 in 5 min).
  Its GPU interrupt pattern still differs from the game's (the driver kicks queued work at its own moments).
- CHD hypothesis ruled out: AITD2 is cue/bin, and CHD decompression flat out (1.4 GB in 60 s) next to the replay
  and audio gave 0.
- **Decision (user): the test is the real pre-fix AITD build on the AITD2 intro** (`run-aitd-old.sh`, ~1 min per
  run). Its rate varies (0-9 per run), so every A/B uses at least 3 runs per condition.

### Phase 2/3 results (2026-11-19, real reproducer: pre-fix AITD build, 3 x 1 min per setting)

| Setting | Underflows (per run) | Total |
|---|---|---|
| Baseline | 3, 7, 2 (and 8 in a later single run) | 12 |
| DE critical threshold 0x40 (kernel code patched in RAM, `de_critical.py`) | 1, 4, 5 | 10 |
| DE critical threshold 0x80 | 3, 9, 2 | 14 |
| GPU at 300 MHz (`pvrsrv/devices` 1005) | 2, 3, 4 | 9 |
| glFinish before every swap (`finishswap.so`) | 4, 6, 5 | 15 |
| DDR GPU port (0xB0290018) 0x0801 + SCH_CTRL bit 2 pulse | 5, 5, 11 | 21 |
| DDR GPU port 0xF801 | 9, 3, 3 | 15 |
| DDR GPU port 0x0001 | 3, 2, 5 | 10 |

All within run-to-run noise; fps 60.3-60.5 throughout. Everything restored afterwards.

Established mechanism (`demon` DE register monitor + ftrace + GL log on one clock, `demon_decode.py`):
- the underflow happens **3.0-3.1 ms into the visible scan** (about line 90), 0.5 ms **before** the flip writes
  any DE register: the flip does not cause it, it only reports it;
- at that moment the GPU is finishing the window render queued by eglSwapBuffers (a 640x480x32 frame, completion
  IRQ at +0.1 ms) and the CPU has just uploaded a 320x200 RGBA5551 texture (-1.4 ms);
- DE MMU is off (DE_MMU_EN = 0, contiguous buffers); both the game and fifostress use `FLIPWSEGL`.

DDR controller (DMM 0xB0290000), from the vendor `bootloader.bin` (LeMaker owl-actions, routine at 0x77bc) and
the S500 datasheet v1.4 (`S500-doc.pdf`, chapter 8 has no register table; DMA chapter: requests carry a
"critical = real-time" bit and a priority weight 2^N):
- vendor init: `+0x0c` (DE) low half = 0xF801, `+0x04` (CPU/BISP) = 0x0818F808, `+0x10` (DE critical) = 0xF8010000,
  `+0x68` = 0x3180, `0xB01B00B4` = 0x3FF; then SCH_CTRL bit 2 cleared and set (load). The GPU, VDE, VCE and DMA
  ports are never programmed (0). Field meanings are inferred, not documented.

### NoC QoS generators (2026-11-19)

The NoC at 0xB0500000 is Arteris FlexNoC. Its **QoS generators** have the layout Rockchip documents (RK3588/PX30
TRMs; mainline `drivers/pmdomain/rockchip/pm-domains.c` saves/restores them): Id_CoreId +0x00 (low byte 0x04 =
QoS generator), Id_RevisionId +0x04, **Priority +0x08** (P0/P1 fields; Rockchip reset 0x80000101), **Mode +0x0C**
(0 fixed, 1 bandwidth limiter, 2 bypass, 3 bandwidth regulator), **Bandwidth +0x10** (threshold, bytes per cycle,
fixed point), **Saturation +0x14** (measurement window), ExtControl +0x18.

Found (read-only, 2026-11-19):
- 0xB0500100: Id 0x8F388204, Rev 0x0121E500, Priority 0xF, Mode..ExtControl 0. The display engine's (the kernel's
  resume code writes 0x0F to its Priority).
- 0xB0500200: Id 0x05B4C304. 0xB0500300: Id 0x9407F104. Owners unknown (GPU, CPU, video, DMA candidates).
- **A blind scan past 0xB0500380 froze the console** (power cycle needed). Only read known offsets of the blocks
  above, one at a time.

Plan: read Priority/Mode/Bandwidth/Saturation of 0x200 and 0x300 (known-valid blocks only); identify the GPU's
generator (a GPU-bound fifostress run while one generator's Mode is set to limiter: the GPU one drops fps); then
A/B the old AITD build with the GPU generator in regulator / limiter mode or at a lower priority. A working
setting becomes a boot-time / GPU-module-init write: a kernel-side fix, no app changes. Rockchip fixed the same
display-underflow class this way (display QoS high, competing masters regulated).

### 16-bit framebuffer (2026-11-19): first lever that works

- NoC QoS is not on the display/GPU memory path: the DE's own QoS generator (0xB0500100) limited to bandwidth 1 for
  3 s gave **0 underflows** (live DE monitor). The DE and the GPU have dedicated DDR-controller ports
  (`DMM_TO_ETHERNET_DE`, `DMM_TO_DE_CRITICAL_USB2`, `DMM_TO_GPU3D_DAP`). NoC blocks 0x200/0x300: all 0; limiter on
  them changed neither GPU fps nor CPU copy speed (0x300 does not even keep Bandwidth/Saturation).
- **fb0 at 16 bpp, with the GPU module reloaded so it picks the format up** (MinUI parked, `pvrsrvctl --stop
  --no-module`, `rmmod pvrsrvkm`, `fbset -depth 16`, `modprobe pvrsrvkm`, `pvrsrvctl --start --no-module`):
  old AITD build **0, 1, 1 underflows (2 in 3 min) vs 12 at 32 bpp**, fps 60.3-60.5 unchanged. The DE reads
  half the bytes per refresh, so its FIFO rides out the GPU + CPU bursts. (Changing fbset alone at runtime fails:
  pvrsrvkm reads the framebuffer format when it loads.)
- To do: confirm on more runs; check colours/size by eye; check every GL client (MinUI, minarch --gl, ports that
  ask for 8888 EGL configs); then make it permanent at boot (fb depth before S03modules loads pvrsrvkm).

### Decision 2026-11-19: no 16 bpp, no live hacks; clean kernel build

The user rejected 16 bpp (it masks the problem, costs colour depth and risks 32-bit GL clients) and further live
patching. Console restored to 32 bpp (`fbset -g 640 480 640 960 32`, pvrsrvkm reloaded, one MinUI).

Kernel source: the running kernel is `3.10.37 (ctalon@endymion) (gcc 4.9.4, Linaro GCC 4.9-2017.01) #40 SMP
PREEMPT Wed Dec 13 2023` (rg35xx-cfw community build, not Anbernic). Candidate exact source:
**github.com/rg35xx-cfw/s500_kernel branch `rg35xx` @ 81cc77b592** (3.10.37, `rg35xx_atm7039_defconfig` and
`rg35xx_atm7039_debug_defconfig`, RG35XX DTS), toolchain Linaro GCC 4.9.4-2017.01 arm-linux-gnueabihf.

- K1 (offline): build the defconfig unchanged; compare with the running kernel (version, kallsyms, bytes of
  de_path_fcr_set / owlfb); check whether the tree has the PVR KM source matching the device's pvrsrvkm.ko.
- K2: same source + tracing (function/function_graph tracer, kprobes, perf, tracepoints in the DE flip /
  fcr_set / underflow path and the GPU display-class flip; DE underflow IRQ if the hardware has one).
- K3 (needs the user's OK): boot it from p1 next to the untouched original (uEnv.txt selects), original as
  fallback; SD restorable from the PC.

#### K1 progress

- Tree fetched (rg35xx-cfw/s500_kernel @81cc77b592). Evidence it is the running kernel's source line: Makefile
  3.10.37; `owlfb_set_par` has `printk("set_par(%d)\n")` (the exact per-flip line the device printed and
  patch_uimage_setpar.py NOPs; LeMaker has a silent DBG); `de_atm7059.c` and `manager.c` byte-identical to the
  LeMaker tree (verified against the device disassembly). `de.c` differs only in DSSDBG -> DSSINFO prints.
- `drivers/gpu/pvr` holds only a Kconfig: the device's pvrsrvkm.ko (DDK 3625561) was built outside this tree; the
  LeMaker tree's eurasia_km at the same DDK is the candidate source (the device module has extra
  `owl_mmu_import_buffer_on_dc`).
- `rg35xx_atm7039_defconfig` already has FTRACE, EVENT_TRACING, TRACEPOINTS, PERF_EVENTS, HW_PERF_EVENTS, DEBUG_FS,
  KALLSYMS_ALL, PREEMPT, DEBUG_INFO (no IKCONFIG; the `_debug_defconfig` adds IKCONFIG plus networking/USB
  options).
- Blocked: Linaro GCC 4.9.4-2017.01 is no longer served by releases.linaro.org; downloading it from the Armbian
  mirror (mirrors.dotsrc.org/armbian-dl/_toolchain/) was refused by the sandbox: needs the user's decision.

#### K1 result: source match and the non-public patch set (2026-11-19)

- Toolchain: the user's `gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf.tar.xz` (repo root, sha256
  22914118...), exact compiler of the running kernel and pvrsrvkm.ko.
- Stock defconfig and a device-equivalent config (rg35xx_atm7039_debug_defconfig + SQUASHFS, HIBERNATION, GAMEPORT,
  JOYSTICK_ANALOG/SIDEWINDER, EARLYSUSPEND, LEDS_PWM, USB_NET_RNDIS_HOST, USB_AUDIO, no IKCONFIG) build cleanly
  (host tools need HOSTCFLAGS=-fcommon). VIDEO_OWL_DSI=m fails modpost (owl_dss_debug not exported): leave it out or
  built in.
- pvrsrvkm.ko rebuilt from LeMaker DDK 3625561 (out-of-tree, M=): same vermagic, 814 of the device module's 816
  functions; the 2 missing are OMAPLFB early-suspend hooks (built when the kernel has EARLYSUSPEND).
- `workspace/src/fifostress/bindiff.py` (function-level diff, address-normalised, opcode-only similarity) found the
  running kernel's **non-public patches** (ctalon / Anbernic "allen" vendor code):
  - only on the device: rumble `show_moto`/`store_moto` (+ `ext_moto_pwm`), charge LED (`led_test`,
    `set_my_charg_led`, `allen_led_poll`), power key polling (`atc260x_poll_hard_onoff`, `hardkey_proc_open`,
    `hard_key_get_state`), audio (`asoc_earphone_poll`, `allen_adjust_volume`, `audio_input_mode_get/put`, ramp),
    PM (`owl_force_shutdown`, upgrade flag proc, `read_current_uref2`, wakeup flag), JTAG sysrq, alarm flags;
  - modified: owlfb (`owlfb_pan_display`, `set_fb_fix`, `owlfb_ioctl`, `owlfb_probe`), owl_dss
    (`dss_mgr_wait_for_go`, `de_video_str_set`, `de_display_type_set`, `owl_default_get_recommended_bpp`,
    `dss_use_replication`), atc260x_power (probe, charging monitor, props), audio dai/pcm (`atm7059_dai_hw_params`
    1106 vs 587 instructions, pcm hw_params, dma trigger/complete), HDMI, sched rt online/offline (likely config).
- The device tree stays the device's own `kernel.dtb` (separate file on p1): no need to rebuild it.
- Order: (1) rumble + power key + charge LED, (2) display/fb flip path, (3) audio, (4) PM/misc; each verified by
  re-running bindiff until the function matches (opcode similarity ~1.0).

#### K2 progress: patch series (workspace/src/kernel-rg35xx/patches/)

- `0001 owl pm: owl_force_shutdown()`: a flag `owl_pm_halt()` checks to power off without wall/VBUS wake-up.
- `0002 atc260x_power: RG35XX board support`: rumble (`moto`), status LEDs + `led_test`, OTG switch, ldo5,
  `no_screen` charge-current mode (on in the device DTS), battery temperature control (off in the DTS), status-change
  log. The charger struct layout now equals the device's (no_screen at +692, `led_work` at +996).
- Verification: `workspace/src/fifostress/skeldiff.py` compares each function's ordered calls and strings (opcode
  ratios mislead when code is refactored or inlined differently). All recovered power functions match; remaining
  differences are the two GPIO/temperature helpers, block order and the device inlining
  `atc260x_disable_vbus_id_if_needed` into the monitor. Intentional deviations: absent/busy pins are skipped and
  shutdown also stops the LED poll.
- Not ported (inert or Android-only on this console, noted so nobody re-hunts them):
  - atc260x-onoff hard power *switch* poll (`atc260x_poll_hard_onoff`, `/proc/hardkey`, `hard_key_get_state`): needs
    `gpios` in the DT `onoff` node, which the RG35XX kernel.dtb does not have; the power button stays on the PMIC
    on/off IRQ path (unchanged).
  - `owl_get/set_alarm_flag` (charger-mode alarm reboot): exported, never called (no caller in the kernel or the
    loaded modules), flag always 0. `alarm_ioctl` RTC power-on alarm ("SSJ"): Android only.
  - `bootdev` "slcnand" boot device: not used (SD boot).
- bindiff false positives: switch jump tables are `$d` in the build but not in the device kallsyms; `--no-data-marks`
  compares both sides the same way (e.g. `owl_default_get_recommended_bpp`, `dss_use_replication` are identical).
- `0003 owlfb: framebuffer layout and flip path`: 2 overlays per fb (`owlfb_info` is 48 bytes, `fbdev` at +40);
  `owlfb_device` gains DT `rotate`/`mode` (+828/+832); DT values read as u32 (the source passed u16 locals to
  `of_property_read_u32`) plus `bpp`; fb memory sized from the panel (fb0 = 5 double-buffered screens = 12288000 B,
  fb1 one = 7372800 B, as in sysfs on the console), overlay reserve 512x512 YUV420; `yres_virtual` defaults to two
  screens; DT `mode = 1` takes `line_length` from xres; fb0's var gets the DT rotation, overlays set up unrotated;
  `owlfb_pan_display` always copies/re-applies (the source's dangling `if (...) //return 0;` guarded the var copy, so
  a pan to a new offset wrote uninitialised stack into `fbi->var`) and passes `!overlay_disable_required` as the new
  third argument of `owlfb_apply_changes()`, which then skips overlay 1 (fb0 drives overlays 0,1; the flag is set by
  the overlay enable/disable ioctl, 0 by default, so each flip re-applies overlay 0 only); no blanket manager apply
  at probe; no `set_par` printk (it is called per GPU flip; the console's uImage had it NOPed on disk).
- `0004 owl dss`: DE MMU table 1 MB (was 4 MB), planar YUV420 swaps chroma for NV21 (the source tested NV12, which
  never reaches that case), HDMI debug printks off.
- Verification of 0003/0004: every function in the display range (owl dss, DE, displays, owlfb, 600 functions)
  compared with `--no-data-marks`-style opcode diff; what remains is register allocation/scheduling, literal pools,
  `__FILE__` paths and WARN line numbers, plus: `owlfb_create_sysfs` (config: the device has
  `CONFIG_FB_DEFAULT_MIRROR_TO_HDMI` off, `mirror_to_hdmi` reads 0), the HDMI driver (real changes in
  `hdmi_send_uevent`, `platform_hdmihw_probe`, `owldss_hdmi_display_*`, `ip_hdmi_*`: next HDMI patch) and
  `owl_pwm_bl_probe` / `owl_backlight_set_onoff` (backlight: to review).
- Kept as in both kernels (not a recovered change): `owlfb_apply_changes` subtracts `overscan_height * 2` from
  both outw and outh; overscan is 0,0 on the LCD.
- Apply the series with `git am --keep-cr` (vendor files mix CRLF and LF).
- Config differences found so far (for the K2 config step): `CONFIG_FB_DEFAULT_MIRROR_TO_HDMI` off.
- `0005 owl pwm backlight`: enable GPIO optional (none in the DTS); `owl_backlight_set_onoff()` returns when no
  backlight is registered (the source dereferenced NULL).
- `0006 owl hdmi` (HDMI unused on this console, ported but not polished): EDID mode check by CEA code (the source
  shifted the bitmap by the vid enum), boot mode kept only if supported, per-mode AVI aspect (`hdmi_cm.aspect`, the
  +4 that shifts every hdmi struct), no strcmp(NULL) without `default_resolution`, info printks off.
- `0007 atc2603c audio` (core only): `allen_adjust_volume()` exported (the rootfs `gpio_keys_polled` gamepad module
  needs it, it would not load otherwise), playback controls with that volume, earphone method 3 poll of both
  board-revision detect pins with speaker PA switching, speaker off at shutdown, no SPDIF (link + Kconfig select).
  Not ported (user decision 2026-11-19, no BT/HDMI use): Bluetooth PCM card device (hw:0,2), S900-style DAI and
  PCM/DMA rework, input-mode control, deferred link init.
- Config (`workspace/src/kernel-rg35xx/config/rg35xx-test.config`): device-equivalent + FB_DEFAULT_MIRROR_TO_HDMI
  off, VIDEO_OWL_DSI=y, SPDIF off (all three as on the console) + KPROBES/KPROBE_EVENT, SCHED_TRACER,
  PREEMPT_TRACER, FTRACE_SYSCALLS (no module ABI change). No function tracer yet (changes `struct module`: needs
  the 4 rootfs modules rebuilt). HZ/preemption unchanged until a baseline trace exists.
- Module compatibility verified: vermagic identical, `struct module` 0x168 as on the console, every symbol of
  pvrsrvkm/drm/gpio_keys_polled/atc260x_cap_gauge (rootfs and p1 copies) resolves (p1's gpio_keys_polled lacks
  `allen_get_cpu_s500` on the console too).
- Build: `workspace/src/kernel-rg35xx/build.sh`; test-boot procedure and tracing quick start:
  `workspace/src/kernel-rg35xx/TEST_BOOT.md`. Boot path: the pre-loader reads p1:/uImage (U-Boot bootcmd=ramboot,
  uEnv.txt not used), so the test swaps p1:/uImage (backup as uImage.koriki).
- `kernel-koriki.patch` (repo root, from the user): the 4 public commits 7390d0c3..81cc77b592 (RG35XX DTS, defconfigs, boot logo off, model name, plus stray scripts/*.o build files). Already in our base; no driver changes. Its DTS is older than the console's kernel.dtb (no `earphone_detect_gpios_new`), so the console's DTB stays the one to boot with.
- Next (K3): test boot with the user's OK, first-boot checks, baseline trace of the underflow; then gaming tuning.

### K3: first traces on the source-built kernel (2026-11-19/20)

Test kernel (patches 0001-0008, test #3) boots with ADB, audio and the rootfs modules. Reproducer unchanged: the
pre-fix AITD build (`/tmp/Tatou.old`), AITD2 intro, `live.sh 60` (fps 60.2-60.5 throughout).

- **Underflows persist on the new kernel** at the same rate as on the stock one: 27 in 12 untraced 60 s runs
  (2,4,1,2,1,2,0,3,8,0,1,3). The counts are strongly over-dispersed (0 to 8 per identical run, clusters at 34-36 s
  of the intro): **a 3-run A/B only detects big effects**; use >= 10 runs per condition or a looped scene.
- Tools (workspace/src/fifostress): `trace-uf.sh` (kprobes on the flip path, the underflow branch
  `de_path_fcr_set+0x250`, `handle_IRQ`, cache maintenance, GPU power/clock gating, CPU hotplug; DMM counters read
  inside the probes; raw pages to /tmp/cap), `trace_uf.py` (per-frame / per-ms bandwidth, underflow timelines),
  `dmm-ids.sh` (which DMM master id carries what), `probe-only.sh` (A/B with single kprobes), `preemptoff.sh`.
- **DDR bandwidth (kernel debugfs `dmm`, owl-ddr-debug.c):** two DMM masters at a time, 16-byte units; the kprobe
  fetch `+0(@0xc0bb1bdc)` / `+0(@0xc0bb1be0)` reads PC0/PC1 through the driver's own mapping (struct `dmm` at
  0xc0bb1b48 in this build). Master ids on the S500: 5 = DE (exactly 74 MB/s = 640x480x4x60), 0 = CPU (~200 MB/s
  RW in the gate scene), **7 ("SI" in the table) = the GPU (~180-200 MB/s RW)**, 6 ("GPU3D") counts nothing. Total
  ~600 MB/s, ~15% of DDR: the load is bursty, not high.
- **GPU traffic per frame is a burst right after vsync**: mean 1.09 GB/s (p99 1.28) 1-2 ms after vsync, 0.59 GB/s
  2-3 ms, then low; the DE restarts its fetch ~1 ms after vsync (end of blanking). But the frame that underflowed
  (one caught with the counters) had an ordinary burst (29th percentile, peak ~0.72 GB/s): GPU bandwidth alone
  does not pick the underflow frame.
- **Ruled out:** CPU cache maintenance (no whole-cache flushes, ~400 small L2 range ops/min, PVR cache ops 7/min);
  GPU runtime power gating (86 power cycles/s, right after vsync; `power/control=on` keeps the domain on: 5 per 3 min,
  no change); DMM performance monitor side effects (11 per 3 min with it on); scheduling (frame periods
  16.48-16.60 ms, worst preempt-off 897 us = idle wake-up on CPU1, not the game); CPU hotplug (none); DDR DVFS/cpuidle
  (not present).
- Interrupts: all on CPU0. SGX ~745/s (12 per frame, p50 17 us), DE pre-line IRQ 60/s busy-waits 378-402 us with
  IRQs off, each SGX IRQ wakes a kworker.
- Single-kprobe A/Bs looked like "probes suppress underflows" (0.3-1 per run) but a never-hit probe and a fresh
  baseline (4 in 3 runs) fall in the same range: not significant (see over-dispersion above).
- DMM registers re-read 2026-11-19 (read-only): unchanged; `DMM_AXI_PRIORITY` (+0x2C) = 0, `+0x28` = 0x202C,
  `+0x68` = 0x3180, `+0x6C` = 1.

#### K3: desampler.ko, 50 us sampling around every vblank (2026-11-20)

`workspace/src/fifostress/desampler/` (module built against the test kernel, `desampler-runs.sh`, `desampler.py`):
at each LCD pre-line callback (start of vblank) it restarts the DMM counters and samples DE_IRQSTATUS,
DE_OUTPUT_STAT, DE_PATH_CTL and the counters every 50 us for 6 ms, then every 500 us; keeps underflow frames, the
frame before, and reference frames. Only PM_CTRL0 works (PC0/PC1); PM_CTRL1 (PC2/PC3) stays 0.
- DE: prefetch ~2 lines at 0.25 ms after the callback, steady fetch from 0.7 ms at 74-81 MB/s.
- 6 underflows captured (6 runs). With DE + GPU (id 7): in all 4, the GPU had **stopped** 0.3-1.3 ms before
  (5-30 MB/s at the underflow). With DE + CPU (id 0): in both, the CPU was **bursting at ~1.0-1.2 GB/s** and the
  underflow bit appeared at the burst's peak; the DE's own rate dipped to 64-65 MB/s in that 50 us interval.
- Sequence: GPU burst ends -> CPU write burst ~1.1 GB/s (texture upload / ghost copy waiting for the GPU) -> DE
  underflow inside it. CPU peaks of 1.4 GB/s also occur without underflow: the address pattern (DDR bank/row
  conflicts with the DE's fetch) likely decides, which fits the run-to-run variance (allocation addresses).
- Next (user-approved, blocked by the Claude Code permission check, to run by hand or allow): DMM CPU port arbitration
  `dmm_port.sh 0x04 lo 0xF801` (CPU low byte 8 -> 1 like the DE), 3 sampler runs, `dmm_port.sh --restore`; then the
  DE side (`0x0c`/`0x10` low byte 1 -> 8).

#### K3: deterministic reproducer and the DE critical path (2026-11-20)

- Sampler + scheduler trace: every AITD underflow burst is the game's own main thread streaming writes (other CPUs
  idle). DMM CPU port E1 (`+0x04` low half 0xF808 -> 0xF801, run by the user): no effect (4 in 3 runs, same CPU peaks).
- **Reproducer (20 s):** `wc-flip.sh 20 --mode set` = fifostress `--fill 1` (vsync-locked flips) + `wcstress`
  (CPU write-combined stores into fb0 screens 6-9, never displayed) at ~1.4 GB/s: **250-752 underflows per 10-20 s**.
  Controls: flips alone 0; WC stores on a static (unflipped) display 0 (sticky DE_OUTPUT_STAT read directly); WC
  memcpy (~0.5 GB/s) with flips 0; the store stream on CPU0 or CPU3 both underflow. GPU idle (1-4 MB/s) in the
  underflow intervals; no GPU power-gate event inside them (but note: `power/control=on` does NOT stop the PVR's
  own gating, so the 2026-11-19 "gating ruled out" A/B was invalid).
- Driver reading: on every flip `de_path_fcr_set` read-modify-writes DE_OVL_CFG critical control (top layer 0,
  others 2) and DE_OVL_CRITICAL_CFG (0x10), then sets FCR; `de_critical_set()` is commented out; `DE_CRITICAL_NUM`
  (DE + 0x08, from regs_map-atm7039.h) is never written and reads 0; DMM DE-critical port = DE port (0xF801).
- **Patch 0009** (`owl dss: run-time tunable DE FIFO critical settings`): `/sys/module/owl_dss/parameters/`
  crit_rewrite / crit_ctl / crit_thresh / crit_num, vendor defaults. Test #4 uImage md5 49cc3bde...
  (`build/uImage-rg35xx-test4`). Sweep: `crit-sweep.sh` (one reproducer run per setting).
