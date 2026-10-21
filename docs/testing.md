# Testing with the OpenXR conformance tests

The server is tested with the [OpenXR conformance test suite](https://github.com/KhronosGroup/OpenXR-CTS)
(CTS) and with WiVRn's own OpenXR tests for the extensions the CTS does not cover. The tests
are grouped in three tiers, by what they need:

| Tier | Runs on | Headset | Graphics | Video encoder |
|---|---|---|---|---|
| `ci` | GitHub-hosted runner (`.github/workflows/CTS.yml`) | virtual | Vulkan on Mesa lavapipe | raw (uncompressed) |
| `gpu` | a host with a GPU | virtual | Vulkan, Vulkan2, OpenGL | the one the server picks (NVENC, VA-API, Vulkan Video, x264) |
| `headset` | a host with a headset connected over ADB | real | Vulkan2 | the one the server picks |

The tiers, the tests they run, the known failures and the extension coverage are in
[`test/cts/suites.toml`](../test/cts/suites.toml); [`test/cts/run_cts.py`](../test/cts/run_cts.py)
runs them.

## Components

- `wivrn-virtual-headset` ([`test/virtual_headset`](../test/virtual_headset)): a headless
  stand-in for the headset. The server only creates its OpenXR system once a headset is
  connected; this program connects with the real protocol (unencrypted), reports a configurable
  set of features (controllers, hand and eye tracking, face and body tracking, refresh rates,
  codecs), answers time synchronisation, sends synthetic tracking at the display rate, sends
  feedback for the video frames and discards them. Its statistics (frames, haptics, refresh
  rate changes...) are written to a JSON file.
- `wivrn-xr-tests` ([`test/xr`](../test/xr)): OpenXR tests for the extensions the CTS does not
  test, end to end: they compare what the application gets (hand joints, face weights, body
  joints, gaze, refresh rates) with what the virtual headset sends, and check that what the
  application sends (haptics, frames) reaches the headset. It takes the same feature options as
  the virtual headset.
- `run_cts.py`: for each run of a tier, starts an isolated server (its own `XDG_RUNTIME_DIR`,
  `XDG_CONFIG_HOME`, D-Bus session and port, so an installed WiVRn keeps running), connects the
  headset, runs `conformance_cli` or `wivrn-xr-tests`, and collects the results.

Both programs are built with `-DWIVRN_BUILD_TEST=ON`.

## Running the tests

```bash
# Server and test programs
cmake --preset server -B build-server -DWIVRN_BUILD_TEST=ON
cmake --build build-server

# The CTS, at the version pinned in suites.toml (needs git-lfs)
test/cts/run_cts.py build-cts --dest openxr-cts

# A tier
test/cts/run_cts.py run gpu --build-dir build-server --cts openxr-cts/build
```

Useful options of `run`:

- `--list`: print the runs of the tier.
- `--run NAME`: only this run (repeatable), `--tests SPEC`: override the test selection (a
  Catch2 test spec for the CTS, comma separated globs for `wivrn-xr-tests`).
- `--vk-driver ICD.json`: Vulkan driver for the server and the tests, e.g.
  `/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` to run the `ci` tier without a GPU.
- `--port`: server port (19757 by default, so it does not collide with an installed WiVRn).
- `--strict`: unexpected passes fail the run.
- `--env KEY=VALUE`: environment for the tests, e.g. `__GLX_VENDOR_LIBRARY_NAME=nvidia` (see below).
- `--wrapper CMD`: run the tests under a command, e.g. `--wrapper "gdb -batch -ex run -ex bt --args"`.

Results go to `cts-results/<tier>-<time>/`: `summary.md`, and per run the server log, the
headset log and statistics, the CTS console output and JUnit report, and `result.json`. In
GitHub Actions the summary is added to the job summary and the directory is uploaded.

The run fails when a test fails that is not listed in `[[expected_failures]]`, when the server
crashes or exits, when the headset received fewer video frames than `min_video_frames`, or when
the runtime advertises an extension that has no entry in `[extensions]`.

### `ci` tier locally

```bash
test/cts/run_cts.py run ci --build-dir build-server --cts openxr-cts/build \
    --vk-driver /usr/share/vulkan/icd.d/lvp_icd.x86_64.json
```

### `headset` tier

The client on the headset must be built from the same tree (the protocol versions must match)
and installed; pass its package with `--package` (`org.meumeu.wivrn.local` by default for a
debug build). The headset must be reachable over Ethernet from the host (pass `--allow-wifi`
otherwise), awake or wakeable over ADB. The script starts the client with a `wivrn://` URI on
the test port, so the headset does not need to be paired (the test server runs with
`--no-encrypt`), and puts it back to sleep at the end.

### Hybrid graphics

OpenGL applications share images with the server, so their context must be on the GPU the server
renders on. On a laptop or desktop with two GPUs, the desktop's OpenGL may run on the other one,
and the `opengl` run crashes in the GL driver while importing the server's images. Select the
server's GPU for the tests, for NVIDIA with `--env __GLX_VENDOR_LIBRARY_NAME=nvidia`, for Mesa
with `--env DRI_PRIME=1`.

## Extension coverage

Every extension the runtime advertises must have an entry in `[extensions]` with the tests that
exercise it. `run_cts.py extensions --build-dir build-server` lists the advertised extensions and
their tests, and each tier run ends with a table of extensions and outcomes. When a new
extension is enabled (in Monado or WiVRn), add a CTS test name or a `wivrn-xr-tests` test for it.

## Adding a test to `wivrn-xr-tests`

```cpp
TEST("WiVRn.XR_FOO_bar", XR_FOO_BAR_EXTENSION_NAME)
{
	instance inst({XR_FOO_BAR_EXTENSION_NAME});
	session s(inst);        // Vulkan session with a projection layer
	s.wait_focused();
	REQUIRE(...);           // stops the test
	CHECK(...);             // records a failure and continues
}
```

The test is skipped when the runtime does not advertise the listed extensions. Expected values
come from the `features` (the virtual headset options) and from the data the virtual headset
sends (see the comments in both programs).

## Known limitations

- XR_EXT_conformance_automation is not implemented by Monado, so the interactive CTS tests (which
  wait for a human to press buttons) are not run.
- Headless sessions (XR_MND_headless) only run `[XR_MND_headless]`: without a compositor, Monado's
  `xrWaitFrame` returns no display time, which the frame-based CTS tests do not support.
- The server ignores head tracking until its clock offset estimation has converged (about one
  second after the connection); until then `xrLocateViews` returns zero orientations. The
  script waits `settle` seconds (3 by default) after the headset connected.
