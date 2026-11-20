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
| `EventStudio.bat [file] [--masks file.masks] [--console]` | Starts the application, optionally opening a recording |

`EventStudio.exe` must run with the OpenEB environment (`EventStudio.bat` sets it): it loads the OpenEB DLLs and
camera plugins from the OpenEB build tree. Integration tests use the OpenEB test datasets in
`C:\Dev\OpenEB_tests\openeb` (`EVS_TEST_DATA_DIR` in `build.bat`); without them those tests are skipped.

Double-clicked, `EventStudio.bat` waits for the application: its console window is hidden once EventStudio starts and
closed when it ends (also in Windows Terminal, whatever its `closeOnExit` setting). Run from a terminal (cmd, PowerShell),
the terminal is left as it is and its prompt comes back when EventStudio closes; `--console` keeps the window.

## Features

**Live camera** (Controls > Source)
- Open the first / a selected camera with the stream format **EVT 3.0** or **EVT 2.1** (changing it reopens the camera).
- **Biases**: Diff ON, Diff OFF, Diff reference, Refractory time, Low-pass filter (`bias_fo`),
  High-pass filter (`bias_hpf`), plus any other bias of the device. The sliders span the range the manufacturer
  allows for the connected sensor (the camera is opened with OpenEB's `biases_range_check_bypass`; without it
  OpenEB rejects values outside the *recommended* range); values outside the recommended range are highlighted.
  Reset to camera defaults, load / save `.bias` files.
- **Hardware filters (ESP)**: Event Rate Controller (enable + rate limit, **on at 40 Mev/s by default**), Event
  Trail Filter (TRAIL / STC_CUT_TRAIL / STC_KEEP_TRAIL, 1–100 ms), Digital Event Mask. Ranges come from the camera.
- **Settings remembered per camera**: the sensor forgets its settings when closed, so the biases, ERC, trail filter
  and trigger inputs EventStudio last gave a camera are saved in `%LOCALAPPDATA%\EventStudio\cameras\<serial>.ini`
  and restored when a camera with that serial is opened again (on this PC only; never in the repository). A camera
  seen for the first time gets the defaults (ERC on at 40 Mev/s).
- **Reset camera**: closes and reopens the camera, restores its settings (and the masks) and restarts streaming.
  It discards the data buffered in the camera / USB driver, so after a saturation the live view is current again
  in seconds instead of minutes. A running recording is stopped.
- **Rate overlay** on the preview: data rate, event rate, camera -> PC delay, ERC status / drop rate, software-filter drop rate.
- **Stream delay** (camera -> PC, see below): current / 10 s max / session max, 60 s plot, warning when the delay
  exceeds the preview accumulation time, CSV log while recording. Also shown while playing a recording (playback
  delay behind the requested speed).

**Recording** (Controls > Recording)
- *Open last recording* (Source panel, Recording panel, File menu): the last file recorded, remembered between
  sessions.
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
events integration; accumulation / decay time, palette, zoom & pan. Measure tool (M, View menu): click a pixel, then
another (Shift: horizontal / vertical) for the distance, dx / dy and angle in pixels; step the time in between (arrow
keys) and the speed in px/ms follows from the times the points were set at. Right click or Esc clears.

**Recordings**: open `.raw` / `.hdf5` / `.dat` (or drop a file on the window), play / pause (Space), seek,
step with Left / Right (paused, by the Step of the playback bar, 100 µs by default; Shift: one accumulation time),
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

## Camera -> PC delay

Event cameras send no absolute time. In EVT 3.0 the timestamp is a 24-bit microsecond counter (12-bit `TIME_HIGH`
and `TIME_LOW` words) that wraps every 16.78 s; the decoder counts the wraps and returns microseconds since the
sensor time base started (stream start). EVT 2.0 / 2.1 are the same apart from a wider `TIME_HIGH`. The only date
in a RAW file is the `% date` header line, written by the PC.

EventStudio therefore pairs clocks: after each USB buffer is decoded, it reads the last decoded camera timestamp
(`Camera::get_last_timestamp()`, which advances with the time words even when the scene sends no event) and the PC
clock, and computes `offset = PC time - camera time`. The smallest offset since the stream started is the
reference; the **delay** of a buffer is its offset minus the reference, i.e. how much later than the fastest
buffer it arrived (camera / USB driver buffering, decoding or the PC falling behind). If no buffer arrives the
current delay keeps growing. Notes:

- The constant part of the latency (exposure to first USB packet) cannot be seen this way; measuring it needs an
  external reference, e.g. a PC-driven signal on the trigger input or an LED filmed by the camera.
- The first second after starting only sets the reference (start-up backlog is not a delay).
- Camera and PC clocks drift apart (typically 10-50 ppm, up to ~0.05 ms per second); in long sessions the
  delay can creep slowly: *Reset reference* in Controls > Source takes a new one.
- *Warn when longer than the accumulation time* (on by default): when the delay exceeds the preview accumulation
  time, the preview shows data older than one frame; the delay turns red and a warning is logged.
- **Playback**: while a recording plays at a paced speed (not *Max*), the same estimate compares the recording
  time / speed with the PC clock. It shows how far playback slips behind the requested speed when decoding and
  the software filters cannot keep up (Controls > Playback, overlay, same warning). Pause, seek and speed changes
  take a new reference.

While a camera is recorded (raw or processed), `<recording>_latency.csv` is written next to the recording, one row
per received buffer: `pc_unix_time_us` (PC UTC clock, us since 1970), `pc_elapsed_us` (since the recording
started), `camera_time_us` (camera time base, as in the events) and `delay_us`. It maps camera time to PC time,
e.g. to align with other instruments: `camera_time_us` happened at about `pc_unix_time_us - delay_us` on the PC
UTC clock, minus the constant latency. With
EVT 3.0, timestamps read back from a RAW file recorded after the stream had run for a while may differ from
`camera_time_us` by a multiple of 2^24 us (the counter period).

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
src/core   camera-independent: filters, polygon / masks, preview renderer, rate meter, latency monitor, EVT
           encoders, RAW writer
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
