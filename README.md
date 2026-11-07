# EventStudio

Desktop tool for Prophesee event cameras built on **OpenEB 5.2** (C++17, Dear ImGui + GLFW/OpenGL).
Live camera control and recording, playback of recordings, software filters, polygon masks and
offline processing / export (event files, video, image sequences).

It replaces the separate OpenEB sample tools (viewer, bias tuning, filtering, recording, conversion) with one
interface, adds software counterparts of the sensor filters that can be applied to recordings, and is the base of
[EBIV Studio](https://github.com/RexAttaque/EBIVStudio), which reuses its event reading, filters and dialogs.

## Requirements

- **Windows 10 / 11, x64.** (The code is mostly portable, but the build scripts and file dialogs are Windows-only.)
- **Visual Studio 2022** or the *VS 2022 Build Tools* with the "Desktop development with C++" workload
  (MSVC 14.3x, CMake, Windows SDK).
- **OpenEB 5.2.0** built from source, with its dependencies installed by **vcpkg 2024.11.16**, following the
  Windows instructions of [OpenEB](https://github.com/prophesee-ai/openeb) (tag `5.2.0`; Prophesee documentation,
  *Installation > Windows > OpenEB*): `vcpkg install --triplet x64-windows --x-install-root installed` with OpenEB's
  vcpkg manifest (`vcpkg-openeb.json`), then `cmake -S openeb -B openeb/build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
  -DBUILD_TESTING=OFF` and `cmake --build openeb/build --config Release --parallel`.
  OpenEB's vcpkg manifest already provides every library EventStudio needs: OpenCV 4 (core, imgproc, imgcodecs,
  videoio), GLFW 3, HDF5, Boost and GoogleTest. Nothing else has to be installed.
- **Prophesee camera driver** (Metavision / OpenEB plugins) only for live cameras; recordings need no camera.

## Installation

The scripts expect this layout (any root folder works, `C:\Dev` is used as the example):

```
C:\Dev\
  openeb\                  OpenEB 5.2 sources, built in openeb\build
  vcpkg-2024.11.16\        vcpkg with OpenEB's dependencies in installed\x64-windows
  openeb_env.bat           copy of openeb_env.example.bat from this repository
  EventStudio\             this repository
```

1. Build OpenEB 5.2 as described above.
2. `git clone https://github.com/RexAttaque/EventStudio.git C:\Dev\EventStudio`
3. Copy `openeb_env.example.bat` to `C:\Dev\openeb_env.bat` (it locates `openeb` and `vcpkg-2024.11.16` next to
   itself and puts their DLLs, HAL plugins and HDF5 plugins on the path; edit it if your folders differ).
4. Run `build.bat`. The first run configures CMake into `build\`, later runs only rebuild.
5. Start the application with `EventStudio.bat [recording]`.

| Script | What it does |
|---|---|
| `build.bat [target]` | Configures (first time) and builds into `build\` (Release, with PDBs) |
| `run_tests.bat [gtest args]` | Runs the unit / integration tests (`build\Release\evs_tests.exe`) |
| `EventStudio.bat [file] [--masks file.masks]` | Starts the application, optionally opening a recording |

`EventStudio.exe` must run with the OpenEB environment (`EventStudio.bat` sets it): it loads the OpenEB DLLs and
camera plugins from the OpenEB build tree. Integration tests use the OpenEB test datasets in
`C:\Dev\OpenEB_tests\openeb` (`EVS_TEST_DATA_DIR` in `build.bat`); without them those tests are skipped.

## Features

**Live camera** (Controls > Source)
- Open the first / a selected camera with the stream format **EVT 3.0** or **EVT 2.1** (changing it reopens the camera).
- **Biases**: Diff ON, Diff OFF, Diff reference, Refractory time, Low-pass filter (`bias_fo`),
  High-pass filter (`bias_hpf`), plus any other bias of the device. Allowed range from the plugin, values outside
  the recommended range are highlighted. Reset, load / save `.bias` files.
- **Hardware filters (ESP)**: Event Rate Controller (enable + rate limit), Event Trail Filter
  (TRAIL / STC_CUT_TRAIL / STC_KEEP_TRAIL, 1–100 ms), Digital Event Mask.
- **Rate overlay** on the preview: data rate, event rate, ERC status / drop rate, software-filter drop rate.

**Recording** (Controls > Recording)
- *Raw sensor stream*: exact camera data (OpenEB `Camera::start_recording`), in the camera format.
- *Processed stream*: what the preview shows (after the software filters), re-encoded in EVT 3.0 / 2.1 / 2.0 by
  EventStudio's own encoders. Also works while playing a file.

**Software filters** (applied to preview, processed recordings and exports), in this order:
masks → activity → refractory → trail → polarity → ERC emulation.
- **Activity (neighbourhood) filter**: keeps an event only if at least N pixels of its (2r+1)² neighbourhood fired
  within ±Δt. *Look-ahead* mode (default) also counts neighbours firing up to Δt later, at the cost of Δt latency;
  the causal mode drops the first event of every neighbourhood, which delays measured particle arrival times.
  On a synthetic 2.4 px particle at 0.5 px/µs with ±20 µs pixel jitter: look-ahead keeps 100 % of the track and
  removes 99.9 % of the background noise; causal keeps 93.5 % and adds a +2.2 µs arrival bias.
- Filter ON / OFF events, event trail filter (same three modes, threshold from 100 µs), refractory period,
  ERC emulation (rate cap per window), masks.

**Hot pixel detection** (Masks > Pixel masks): measures every pixel's rate (live for N s, or by scanning the
recording) before the software filters, and flags pixels above a minimum rate that are much more active than
their third-most-active neighbour (isolated pixels and pairs are flagged, the edges of active regions are not).
The hottest pixels fill the sensor Digital Event Mask slots, the rest become software pixel masks.

**PTV planner** (View menu): scale, particle image size (diffraction-limited), pixel dwell time, event rate vs the
camera limit, maximum seeding density, timing blur from the pixel response, and a suggested activity window
(one click applies it).

**Masks** (Masks window + preview), three categories, saved together in a `.masks` file
(`EventStudio.bat rec.raw --masks file.masks` loads one at start):
- *ROI / RONI rectangle (sensor)*: one rectangle, ROI (only the inside is active) or RONI (the inside is
  inactive), programmed with `I_ROI` when a camera is open. Drawn, moved and resized (8 handles) on the preview,
  or typed in.
- *Pixel masks (sensor)*: click pixels on the preview (a magnifier with a pixel grid follows the cursor) to put
  them in the sensor's Digital Event Mask; limited to its capacity (64 on the IMX636).
- *Software masks (polygons)*: any shape and size, software only. Toggle, rename, duplicate, delete; drag
  vertices, move, scale (corner handle / Ctrl+wheel), rotate (round handle / Shift+wheel), insert (double click
  on an edge) and delete (right click) points; numeric transforms in the panel.
- Undo / redo across all categories. All enabled masks are also applied in software (preview, processed
  recordings, exports): this emulates the sensor masks on recordings; the export panel selects the categories.

**External triggers** are kept everywhere: raw and processed recordings, RAW exports (merged in time order with
the CD events), HDF5 exports, CSV exports (`<name>_triggers.csv`: `p,id,t`). Filters never remove triggers. Trigger
inputs of a live camera are enabled in Hardware filters > External trigger inputs.

**Preview** modes: CD accumulation, time decay, time surface, event frame histogram, event frame difference,
events integration; accumulation / decay time, palette, zoom & pan.

**Recordings**: open `.raw` / `.hdf5` / `.dat` (or drop a file on the window), play / pause (Space), seek,
speed 0.01x–10x or unpaced, loop.

**Process & export**: re-run a recording through the software filters (optionally a time range) and write
RAW EVT 3.0 / 2.1 / 2.0, HDF5, CSV, an AVI video (rendered with the current preview settings) or a **PNG image
sequence**: one image per preview accumulation time (image *i* holds the events of [i·dt, (i+1)·dt), no event is
shared or lost between images), rendered with the preview mode and palette, plus `frames.csv` (index, file, time
range in µs). The default folder is `<recording>_images_<options>` next to the recording, where the options
describe the settings compactly, e.g. `dt-20ms_sERC-50M_masked` (frame period, preview mode if not the CD frame,
time range, software filters with an `s` prefix: `sAct` activity, `sRefr` refractory, `sTrail` / `sSTCcut` /
`sSTCkeep` trail, `ONonly` / `OFFonly`, `sERC` rate control, then `masked`).

**Samples** menu: launches the OpenEB sample applications on the current recording (or on the camera, which is
closed in EventStudio first): viewer, time surface, events integration, event frame generation, filtering,
composed viewer, get started, active pixel detection, dummy radar, stream slicer, file info.

## What can and cannot be applied after recording

| Setting | Offline | Notes |
|---|---|---|
| ON / OFF polarity filter | exact | |
| Polygon masks | exact | |
| Trail filters (TRAIL, STC_*) | exact model | Burst = same-polarity events each within the threshold of the previous one |
| Event rate control | approximation | Per-window cap with even decimation; the sensor's exact dropping pattern is not public |
| Refractory period | approximation of `bias_refr` | Can only remove events, never recreate suppressed ones |
| Biases (diff, diff_on/off, fo, hpf) | **not possible** | They change the analog pixel response; the information is not in the recording |

## IMX636 hardware limits (and how EventStudio handles them)

- **Digital Event Mask holds 64 pixels.** When the enabled masks cover ≤ 64 pixels they are also written to the
  sensor; larger masks are applied in software only, so *raw* recordings still contain those pixels (use the
  *processed* recording mode or an export).
- **Trail filter threshold: 1–100 ms in 1 ms steps** in hardware. Shorter windows (down to 100 µs) are only
  available with the software trail filter.
- **ERC drop counters**: the sensor's ERC exposes no readable input / dropped counts. If the device sends ERC
  counter events the overlay shows the exact drop rate; otherwise it shows the output rate relative to the limit.
- No hardware polarity filter: ON / OFF filtering is software only.

## Layout

```
src/core   camera-independent: filters, polygon / masks, preview renderer, rate meter, EVT encoders, RAW writer
src/io     OpenEB-based: live camera, file reader / player, offline processor, sample launcher
src/app    ImGui application (panels, preview / mask editor, platform dialogs)
tests      GoogleTest suite (encoders are verified by decoding with the OpenEB decoders, EVT3 with the robust
           grammar-checking decoder)
third_party/imgui  Dear ImGui v1.91.5-docking (vendored)
```

## Known limitations

- Live-camera code paths are untested on hardware in this environment (no camera connected during development).
- EVT 2.0 exports whose timestamps exceed 2^34 µs (~4.8 h) may be decoded modulo 2^34 µs by OpenEB (its EVT 2.0
  decoder detects time loops only in dense streams). EVT 2.1 / 3.0 do not have this limitation.
- A trigger arriving more than 20 ms after later CD events during a live *processed* recording would be stamped
  with the latest written time (decoders deliver them within the same buffer, so this is not expected).
