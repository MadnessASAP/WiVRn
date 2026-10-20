# XR_EXT_frame_synthesis auxiliary data transport

Status: experimental, implements phases 0 to 3 of the SGSR2 offload roadmap
(`WiVRn_SGSR2_Implementation_Roadmap.md`). The headset receives and decodes the
depth and motion vector images of a cooperating OpenXR application, in-band with
the video stream. Nothing consumes them yet: SGSR2 (phase 5) and the modified
test application (phase 4) are not implemented.

## 1. Overview

```text
OpenXR app (Vulkan)
  xrEndFrame: XrCompositionLayerProjectionView
                └─ XrFrameSynthesisInfoEXT (motion vector + depth sub images)
      │  Monado state tracker: validation, then xrt_compositor::layer_frame_synthesis
      │  Vulkan client compositor → IPC (shared memory) → multi compositor → comp_base
      ▼
WiVRn compositor::layer_commit (single projection layer only)
  frame_synthesis_pack.comp: copy/downsample to a host visible buffer (same submit as foveation)
      ▼
encoder thread, per eye: filter + zstd, serialize view_header + payload
  video_encoder::send_aux_data: SEI NAL units, sent before the coded picture of that eye
      ▼
existing video shards (frame_idx, shard_idx), UDP or TCP
      ▼
headset shard_accumulator: SEI shards removed before the video decoder, reassembled
  frame_synthesis_decoder (worker thread per eye): parse, decompress, upload to VkImages
  stream::push_blit_handle: attached to the decoded colour frame with the same frame_idx and eye
```

The colour stream, its decoders and the existing rendering path are unchanged
when no auxiliary data is present, when the feature is disabled on either side,
or when the data of a frame is invalid.

## 2. Phase 0: reconnaissance

### Versions

| Component | Version |
|---|---|
| WiVRn | `e599b381` (v26.9-82) plus this work |
| Monado | `8b259763fdd23c46592fd4cdd30b4eea997d4b70` (`monado-rev`) with `patches/monado/0001`..`0011`, plus `0012-st-oxr-add-XR_EXT_frame_synthesis-plumbing.patch` |
| OpenXR headers | Monado bundled headers (1.1.x, `XR_EXT_frame_synthesis` revision 1) |
| zstd | system `libzstd` on Linux, 1.5.7 downloaded on Android |

### Findings

* `XR_KHR_composition_layer_depth` is disabled in WiVRn (`XRT_FEATURE_OPENXR_LAYER_DEPTH OFF` in
  `server/CMakeLists.txt`). The compositor fast path accepts `XRT_LAYER_PROJECTION_DEPTH`
  layers but only uses the colour images: no depth ever reached the headset.
* Monado had no `XR_EXT_frame_synthesis` support at this revision. Projection layers travel
  through 5 hops (state tracker, client compositor, IPC client/shared memory, IPC server, multi
  compositor) before reaching `comp_base`, each with its own layer storage, all of which had to
  be extended (see section 5).
* WiVRn's compositor is a `comp_base` implementation: `compositor::layer_commit` has a fast
  path for a single projection layer that feeds the application images directly to the
  foveation compute shader, other cases go through `layer_squasher`.
* Foveation remaps the colour image only when the application image is larger than the
  encoded size. The parameters are sent per frame in `view_info_t::foveation`.
* Each encoder produces an Annex B bitstream through `video_encoder::SendData`, which splits
  data in shards of at most 1400 bytes; every `SendData` call starts a new shard. The first
  shard of a frame carries `view_info`, the last one `timing_info`.
* The headset `shard_accumulator` pushes contiguous shards to the decoder as they arrive. The
  Android decoder copies them into a MediaCodec input buffer of fixed capacity, and drops the
  frame if the data does not fit: large SEI must be removed before the decoder.
* The protocol version is a hash of all packet types (`common/protocol_version.h`): any change
  to the packets requires matching client and server builds, as for any WiVRn release.

### Ownership boundaries

| Area | Files |
|---|---|
| OpenXR validation and submission | Monado `src/xrt/state_trackers/oxr/oxr_session_frame_end.c`, `oxr_system.c`, `extension_support/oxr_extension_support.py` |
| Compositor interface | Monado `src/xrt/include/xrt/xrt_compositor.h` |
| Layer transport | Monado `compositor/client/comp_vk_client.c`, `ipc/shared/ipc_protocol.h`, `ipc/client/ipc_client_compositor.c`, `ipc/server/ipc_server_handler.c`, `compositor/multi/comp_multi_*.c`, `compositor/util/comp_layer_accum.*`, `compositor/util/comp_base.c` |
| Capture | `server/compositor/frame_synthesis.{h,cpp}`, `server/compositor/shaders/frame_synthesis_pack.comp`, `server/compositor/compositor.cpp` |
| Encapsulation | `server/encoder/video_encoder.{h,cpp}` |
| Codec-neutral format, SEI | `common/frame_synthesis/` |
| Negotiation | `common/wivrn_packets.h`, `client/scenes/stream.cpp`, `server/compositor/compositor.cpp` |
| Headset extraction and decoding | `client/decoder/shard_accumulator.*`, `client/decoder/frame_synthesis_decoder.*`, `client/decoder/decoder.h` |

### Baseline measurements

Recorded on 2026-10-18 with the released WiVRn 26.9 server and client (the protocol of this
branch is not compatible with the released client), `hello_xr -g Vulkan2` as the application.

| Item | Value |
|---|---|
| Host | RTX 2060 (driver 615.78.08, one NVENC), Ethernet to the access point |
| Headset | Quest 3, Android 14 build `UP1A.231005.007.A1` (52433670048800520) |
| Wi-Fi link | Wi-Fi 6, 5785 MHz (channel 157), RSSI −25 dBm, 1200 Mbit/s PHY both ways |
| Application render size | 2476×2594 per eye (recommended size with the client resolution scale) |
| Encoded size | 1280×1344 per eye, H.264 8-bit NVENC, 98.7 Mbit/s target per eye; alpha 1280×672, 2.4 Mbit/s |
| Refresh rate | 120 Hz (client setting); colour is foveated (encoded size ≈ 0.52× the render size per axis) |
| Host GPU | NVENC 71 % average, 3D engine 14 % average (`nvidia-smi dmon`, 60 s) |
| Network | 5.3 Mbit/s median: the `hello_xr` scene is static, this does not exercise the bitrate |
| Headset (VrApi, 60 s) | 120/120 FPS, 0–1 stale frames per second, compositor GPU 29–31 % at CPU/GPU level 4/4, prediction 19–25 ms, 33 °C |
| Client log | no dropped or incomplete frames in 60 s |
| RTT host→headset | 1.2–4.7 ms (2.5 ms average) while streaming; 2–117 ms when the headset is idle (Wi-Fi power save) |

Network capacity host→headset (iperf3 3.17.1 built with NDK r30, 1400-byte UDP datagrams as
WiVRn shards, sent in bursts of one 90 Hz frame):

| Rate | 224 KiB socket buffer (Android default) | 5 MiB (requested by the WiVRn client) |
|---|---|---|
| 100 Mbit/s | 0.26 % loss | 0 % |
| 200 Mbit/s | 0.57 % | 0 % |
| 300–600 Mbit/s | 0.68 % (300) | 0 % up to 600 Mbit/s |

TCP: 705 Mbit/s without retransmissions. Losses with small buffers come from the receive
socket buffer overflowing on bursts, not from the radio; the WiVRn client sets a 5 MiB buffer
(`rmem_max` is 8 MiB on the headset), so it is not affected.

Not measured: per-stage latencies (encode, transmit, decode) of the released client, which are
only shown in the headset statistics tab, and a high-bitrate scene.

### Headset decoders

From `/vendor/etc/media_codecs*.xml`:

| Decoder | Limits |
|---|---|
| `c2.qti.avc.decoder` | up to 8192×8192, 7.78 M macroblocks/s (4K at 240 fps), 220 Mbit/s, 16 instances |
| `c2.qti.hevc.decoder` | up to 8192×8192, 7.78 M macroblocks/s, 160 Mbit/s, 16 instances |
| `c2.qti.av1.decoder` | measured up to 505 fps at 3840×2160 |
| `*.low_latency` variants (AVC, HEVC, AV1, VP9) | about half the throughput (AVC: 3.9 M macroblocks/s, 70 Mbit/s) |
| `c2.qti.mvhevc.decoder` (`video/x-mvhevc`) | multi-view HEVC, up to 4096×4096, **60 fps maximum**, 2.07 M macroblocks/s, 8 instances |

An MV-HEVC decoder exists, but it is for stereo multi-view video: nothing indicates support for
HEVC auxiliary (alpha/depth) layers, and its 60 fps limit is below the streaming refresh rates.
Additional regular decoder instances for auxiliary video streams are within the limits (16
instances, 2 streams of 1280×1344 at 120 Hz use about 13 % of the AVC throughput).

## 3. Phase 1: transport contract

### 3.1 Codec and encapsulation choice

First codec: **H.264**, with the identical mechanism for H.265. AV1 and raw streams never
carry auxiliary data.

Auxiliary images are compressed independently of the video and carried as **user data
unregistered SEI messages** (payloadType 5) in the access unit of each eye, extracted by the
headset before the hardware decoder:

* The Android NDK MediaCodec API returns one decoded picture per access unit. There is no API to
  obtain H.264 auxiliary coded pictures or HEVC multi-layer (alpha/depth auxiliary) pictures as
  separate images, so native auxiliary layers are not usable on Quest 3 through the API WiVRn
  uses. They were not tested on device.
* Packing depth and motion in the colour picture would subject them to 4:2:0 chroma
  subsampling, lossy rate control and foveation.
* SEI is codec-legal and ignored by decoders. Splicing the SEI produced by this implementation
  after each access unit delimiter of an x264 stream leaves the FFmpeg decoded frames
  bit-identical, and FFmpeg's `trace_headers` parses every fragment as a
  `user_data_unregistered` message.

The cost is a CPU readback of the (downsampled) images on the host and a CPU decompression on
the headset.

### 3.2 Bitstream layout

For each eye stream (0: left, 1: right), when a frame has auxiliary data, the access unit
starts with one or more SEI NAL units, followed by the encoder output for that frame:

```text
00 00 00 01                         start code
06            (H.264)               nal_unit_type 6, nal_ref_idc 0
4e 01         (H.265)               PREFIX_SEI_NUT, layer 0, temporal id 1
05                                  payloadType: user data unregistered
ff ... xx                           payloadSize
8b 2f 5d c1 47 e6 4a 93 b5 1c 6e d2 39 a7 f4 58    uuid_iso_iec_11578 (no zero byte)
01                                  fragment header version
ii ii                               fragment index (little endian)
nn nn                               fragment count
ss ss ss ss                         total blob size
...                                 fragment data, at most 60000 bytes
80                                  rbsp_trailing_bits
```

Emulation prevention bytes are inserted after the NAL header. The payloadSize field and the
UUID never need them, so a NAL unit can be identified from its first bytes without unescaping.

The concatenation of the fragment data of all NAL units of the access unit is the auxiliary
blob of that eye: a serialized `view_header` (226 bytes, WiVRn serialization) followed by the
motion vector payload then the depth payload.

Each SEI NAL unit is passed to its own `SendData` call, so it starts on a shard boundary and
no shard contains both auxiliary and video data. The NAL units are sent synchronously before
the encoder output of the frame, so the last shard (with `timing_info`) is always video data.
The headset relies on this: a shard starting with a start code begins a new NAL unit, which is
auxiliary data if its header matches the above; other shards continue the previous NAL unit
(a continuation cannot start with a start code because of emulation prevention).

### 3.3 Frame descriptor

`common/frame_synthesis/aux_frame.h`, one per eye and frame:

| Field | Meaning |
|---|---|
| `version` | `frame_synthesis::format_version`, currently 2 |
| `frame_index` | WiVRn frame index, identical to the video shards `frame_idx` |
| `view_index` | 0 left, 1 right, identical to the stream index |
| `display_time` | headset clock, identical to `view_info_t::display_time` |
| `app_display_time`, `previous_app_display_time` | headset clock: display time of the application frame shown, and of the application frame the motion is relative to (the session's previous `xrEndFrame`, 0 if none) |
| `previous_frame_index` | WiVRn frame index of the previous frame sent if it showed that previous application frame, else `no_previous_frame` (`UINT64_MAX`): the motion describes the change from that frame to this one |
| `flags` | `color_foveated`: colour pixels are not linearly mapped to the view; `downsampled_closest_depth` |
| `layer_flags` | `XrFrameSynthesisInfoEXT::layerFlags` unmodified (2D motion bit, relaxed frame interval bit) |
| `motion_vector_scale`, `motion_vector_offset` | xyz of the OpenXR values, full precision (w is ignored by the specification) |
| `app_space_delta_pose` | `appSpaceDeltaPose` unmodified, in the projection layer space |
| `layer_space_pose` | pose of the projection layer space, in the space of `view_info_t::pose` |
| `min_depth`, `max_depth`, `near_z`, `far_z` | OpenXR values, full precision, infinity allowed |
| `color_x/y/width/height` | application colour sub image rectangle of this view |
| `motion_encoding`, `motion` | wire encoding and `image_info` of the motion vector image |
| `depth_encoding`, `depth` | wire encoding and `image_info` of the depth image |

`image_info` holds the application sub image (offset, extent, array index, VkFormat), the wire
size, filter, compression, uncompressed and compressed sizes.

The serialized header layout is covered by `frame_synthesis::format_hash()` (a hash of the
structure and version, currently `0x1301f71272035078`), used for negotiation.

### 3.4 Pixel encoding and precision

| Image | Wire encoding | Content |
|---|---|---|
| Motion, 2D (`USE_2D_MOTION_VECTOR`) | `rg16_sfloat`, 4 bytes/pixel | raw texel rg |
| Motion, 3D (default) | `rgba16_sfloat`, 8 bytes/pixel | raw texel rgb, a = 0 |
| Depth | `r32_sfloat` (default) or `r16_sfloat` (`depth-16bit`) | raw depth value |

Values are the application texel values before `motionVectorScale`/`motionVectorOffset`, and
depth is not linearized, so the OpenXR semantics are kept: the receiver computes
`motion = rgb * scale + offset`. This is lossless for the recommended `R16G16B16A16_SFLOAT`
motion vector format, for `D16` and `D32` depth with `r32_sfloat`, and almost lossless for
`D24`. 3D motion is never silently reduced to 2D.

The wire images both have the resolution of the depth sub image times `scale` (default 0.5,
0.25 to 1). When downsampling, each wire pixel takes the closest depth sample of its footprint
(the smallest value, or the largest when `near_z > far_z`) and the motion vector at that same
position, so foreground edges and their motion are preserved. A footprint is sampled with at
most 4×4 samples.

Accepted application formats: motion vectors in any `R...` colour format with float, unorm or
snorm components, not sampled as sRGB; depth in any depth format. Other formats are not
transmitted (logged once).

### 3.5 Compression

Each image is filtered then compressed with zstd (level 1 by default):

* filter `delta_byte_planes`: on the packed, component-interleaved stream, each component bit
  pattern is replaced by its difference (wrap-around) with the same component of the pixel on
  the left, or the pixel above for the first column; the result is split in byte planes (all
  low bytes, then the next bytes...).
* the decoder checks the zstd frame content size against the expected size before
  decompressing.

Measured on a desktop CPU (single thread, 720×768 per eye, i.e. a 1440×1536 application image
at `scale` 0.5, 32-bit depth, 2D motion):

| Content | Compressed size per eye | Compress (host) | Decompress |
|---|---|---|---|
| Piecewise planar synthetic scene | 17 KB (motion 3 KB, depth 14 KB) | 3.5 ms | 3 ms |
| Same with per-pixel noise (worst case) | 1.7 MB | 9 ms | 5.5 ms |
| Uncompressed | 4.4 MB | | |

Real scenes are expected between these. Decoding runs on one worker thread per eye, off the
network and render threads. Measured on the Quest 3 CPU (same code built with NDK r30, headset
idle, 40 iterations):

| Wire size per eye | Content | Payload | Decode per eye, median / p90 |
|---|---|---|---|
| 720×768 (scale 0.5) | piecewise planar | 18 KB | 3.45 / 3.86 ms |
| 720×768 | with noise | 1.7 MB | 7.29 / 8.08 ms |
| 1440×1536 (scale 1) | piecewise planar | 38 KB | 20.2 / 23.8 ms |
| 1440×1536 | with noise | 6.8 MB | 38.2 / 42.9 ms |

The frame period is 8.3 ms at 120 Hz and 11.1 ms at 90 Hz: the CPU path only fits at
`scale` 0.5 or below, and is close to the limit at 120 Hz.

### 3.6 Negotiation and enabling

* The headset sends `headset_info_packet::frame_synthesis_format = format_hash()`, and
  `settings_changed::frame_synthesis` (also in the initial `headset_info_packet::settings`) from
  the "Depth and motion vectors" setting (off, receive only, headset frame synthesis), default
  off, which can be changed while streaming: the data is requested unless it is off.
* When the headset runtime supports `XR_EXT_frame_synthesis`, the headset also sends its
  recommended motion vector image size (`headset_info_packet::frame_synthesis_motion_vector_width`,
  `_height`, 420×440 on Quest 3), which the server advertises to applications in
  `XrFrameSynthesisConfigViewEXT`; otherwise applications are told the colour size.
* The server transmits auxiliary data for a frame only when: the server configuration
  `frame-synthesis` is enabled, the headset format hash matches, the headset requests it, both
  eye streams are H.264 or H.265, the frame has a single projection layer with
  `XrFrameSynthesisInfoEXT` on both views, and the images are not flipped.
* The OpenXR extension is advertised whenever the runtime is built with it; when the data is
  not transmitted it is dropped. This is allowed: the data is a hint for the runtime.

### 3.7 Loss and validity policy

* A complete stereo source frame with reconstruction inputs is: decoded colour frames of both
  eyes with the same `frame_idx`, each with an attached `frame_synthesis` object in `ready`
  state, whose `header.frame_index` and `view_index` match, and without the `color_foveated`
  flag. Anything else must fall back to the existing path.
* Auxiliary data shares the fate of its video frame: shards are only assembled for complete
  frames, a frame with a missing shard is discarded entirely. The data of a frame is never kept
  for another frame.
* Each auxiliary frame is self-contained (no prediction across frames): reception recovers on
  the next frame, independently of IDR frames.
* Rejected on the headset (counted as errors, colour unaffected): incomplete or inconsistent
  fragments, malformed header, unsupported version, inconsistent sizes, sizes above limits
  (4096 pixels per side, 8 MiB per payload, 16 MiB per blob), invalid depth range or near/far,
  non-finite parameters, corrupted zstd data, frame index or view index different from the
  frame it arrived with.
* The server does not send a frame whose layers all show the same application frames as the
  last frame sent (the application did not submit a new frame in time), so frame indices have
  gaps. The headset resubmits its last frame, except in headset frame synthesis mode once data
  was forwarded: it then submits no frame, so the runtime synthesizes the missing ones from the
  forwarded data (Quest: Application SpaceWarp, in its half rate mode). For tests,
  `debug.wivrn.discard_repeated_frames true` discards in all modes.
* Not sent by the server: data larger than `max-view-kib` for an eye (default 2 MiB), frames
  with several layers, unsupported formats.

### 3.8 Coordinate and transform semantics

* Motion vectors: `CurrNDC - PrevNDC` in Vulkan NDC (x right, y down, z in [0, 1]), between the
  `displayTime` of the previous and current application frames, after scale and offset. A
  displacement of 2 in x is the full width of the view. Whether camera motion is included is
  defined by the application (OpenXR: all pixel movement).
* Wire pixel (x, y) of an image of wire size (w, h) covers the source pixels
  `[source_x + x * source_width / w, source_x + (x + 1) * source_width / w)` (same for y) of the
  application sub image, and holds the closest sample of that range.
* The motion and depth sub images are assumed to cover the same field of view as the colour sub
  image of the view (`color_*`), with `view_info_t::fov`. Colour image pixel coordinates map to
  NDC linearly only when `color_foveated` is not set.
* Depth: raw value d in `[min_depth, max_depth]`, with `min_depth` at distance `near_z` and
  `max_depth` at `far_z`; `near_z > far_z` means reversed depth; either may be infinite.
* Poses: `view_info_t::pose` and `layer_space_pose` are in the same space. With L =
  `layer_space_pose` and D = `app_space_delta_pose` (expressed in layer space), the application
  transform in that space is `L · D · L⁻¹`.

## 4. Phase 2: headset implementation

* `shard_accumulator::try_submit_frame` classifies shards in bitstream order; auxiliary shards
  go to a `sei_reassembler` of the frame instead of the decoder. When the frame is complete,
  the blob is passed to the eye's `frame_synthesis_decoder` **before** the colour frame is
  given to the decoder, so the auxiliary object exists when the decoded image is pushed.
* `frame_synthesis_decoder` creates a `frame` object in `pending` state, then on its worker
  thread validates, decompresses into a staging buffer, copies into two images
  (`R16G16_SFLOAT`/`R16G16B16A16_SFLOAT` and `R32_SFLOAT`/`R16_SFLOAT`), waits for the copy and
  sets the state to `ready` (images in `SHADER_READ_ONLY_OPTIMAL`; a timeline semaphore value is
  also provided). Images come from a pool of 8 slots per eye; a slot is reused only when the
  last reference to its `frame` is released. Images replaced after a size change are destroyed
  later. Frames keep the pool (images, semaphore) alive, so they remain valid when the decoders
  are recreated on a stream reconfiguration.
* `stream::push_blit_handle` attaches the object to `blit_handle::frame_synthesis` of the same
  eye and `frame_idx`. It may still be pending: consumers must check `status()`.
* Statistics per eye (received, decoded, errors, dropped, average size, decode time) are shown
  in the headset statistics tab.
* Headset frame synthesis mode (`stream_frame_synthesis`, client): when both eyes of a colour
  frame submitted for the first time have `ready` data of that frame, the images are copied
  (resampled, nearest) into motion vector (`R16G16B16A16_SFLOAT`) and depth (first supported of
  D32, D24S8, X8D24, D16) swapchains of the runtime's recommended size, 2 array layers, by a
  full screen pass in the stream command buffer, which waits for the decoder semaphores. An
  `XrFrameSynthesisInfoEXT` is chained to each view of the stream projection layer: the header
  values, `appSpaceDeltaPose` converted to the layer space (`L·D·L⁻¹` with `layer_space_pose`),
  without `REQUEST_RELAXED_FRAME_INTERVAL` (the stream drives the frame rate). Other frames are
  submitted without it, also when `previous_frame_index` is not the last colour frame submitted
  for the first time (a frame was dropped in between, or the motion is not relative to it):
  the runtime then would apply the motion over the wrong interval. Counters (forwarded, no
  data, not ready, mismatch, repeated, not consecutive) are logged every 10 s and shown in the
  statistics tab. For tests, `adb shell setprop
  debug.wivrn.frame_synthesis_override off|receive|runtime` overrides the setting and
  `debug.wivrn.frame_synthesis_flip_y true` negates the Y component of the motion vectors.
* Debugging real data: run the server with `WIVRN_DUMP_VIDEO=/tmp/stream`, then
  `wivrn-frame-synthesis-dump h264 /tmp/stream-0.h264 /tmp/fs` (built with `WIVRN_BUILD_TEST`)
  prints the metadata of every auxiliary frame and writes the images as PFM files, using the
  same parser and decoder as the headset.

## 5. Phase 3: host implementation

### Monado patch (`0012`)

* Feature `XRT_FEATURE_OPENXR_FRAME_SYNTHESIS` (default OFF upstream, ON in WiVRn).
* `xrEndFrame` validation per view: valid unflagged bits, both sub images (non-null, released,
  image index, array index, face count, rectangle), `appSpaceDeltaPose`, `minDepth`/`maxDepth`
  in [0, 1] with min ≤ max, `nearZ`/`farZ` positive (infinity allowed), `nearZ != farZ`, and all
  or none of the views. Failures return `XR_ERROR_VALIDATION_FAILURE`, `XR_ERROR_HANDLE_INVALID`,
  `XR_ERROR_LAYER_INVALID`, `XR_ERROR_SWAPCHAIN_RECT_INVALID` or `XR_ERROR_POSE_INVALID`.
* New optional `xrt_compositor::layer_frame_synthesis(mv_xsc[], d_xsc[], data)` attaching
  `xrt_layer_frame_synthesis_data` to the last projection layer. Swapchains are stored at
  offsets `2 * XRT_MAX_VIEWS` and `3 * XRT_MAX_VIEWS` of the per layer swapchain arrays, which
  are enlarged to `4 * XRT_MAX_VIEWS`; the multi compositor holds references until the frame is
  retired, like the colour swapchains.
* Implemented by the Vulkan client compositor, IPC client and server, multi compositor and
  `comp_base`. Other client compositors (OpenGL, D3D) do not implement it: their data is
  dropped.
* `XrFrameSynthesisConfigViewEXT` recommends `xrt_view_config_properties::recommended_motion_vector`
  when set (the WiVRn server sets the headset runtime's size), the colour recommended size
  otherwise: the transmitted size is decided by the server.

### WiVRn server

* `compositor::record_frame_synthesis` runs in the fast path (single projection layer), after
  foveation in the same command buffer. The application images are sampled through the
  swapchain image views, in `SHADER_READ_ONLY_OPTIMAL` as for colour; the GPU writes to a host
  visible buffer per compositor image (allocated on first use, sized for the transmitted
  images), made visible to the host by a barrier. The GPU time is reported as
  `frame synthesis capture`.
* In the encoder thread, `video_encoder::encode` calls the provider only if the frame is not
  skipped; the provider waits for the compositor submission of that image (the encoder thread
  is woken before it completes), compresses, and the SEI NAL units are sent before encoding.
  Failures there only drop the auxiliary data, never the colour frame. Compression of an eye
  overlaps the hardware encode already submitted. Traced as `frame synthesis compress` and
  `send frame synthesis data`.

## 6. Limitations and next steps

* The auxiliary data transport itself has not run on the headset: it needs a client built from
  this branch (protocol change) and the phase 4 application. Baselines, link capacity and the
  Quest decode cost have been measured (sections 2 and 3.5).
* Only Vulkan applications with a single projection layer, not flipped.
* Streaming foveation is not disabled automatically: frames with foveated colour carry the
  `color_foveated` flag and must not be used for reconstruction. For reconstruction the
  application renders below the encoded size, which disables foveation.
* The SEI NAL units are sent synchronously by the encoder thread, delaying the network send of
  the next eye by the send time of the auxiliary data.
* A decoder slot is reused as soon as its last `frame` reference is dropped, which is not tied
  to the completion of the GPU work sampling it. Before SGSR2 samples these images, references
  must be held until the render submission using them has completed.
* When auxiliary data is sent, the `send_begin` timing reported to the server, and the network
  trace span, start with the auxiliary data, before the end of the encode.
* Auxiliary shards make a video frame larger, a lost shard makes the whole frame (colour
  included) incomplete, so the frame loss rate grows with the auxiliary data size.
* CPU costs (section 3.5) are significant; a GPU decoder on the headset or lossy quantization
  of motion vectors are possible improvements once real scenes are measured.
* xrizer and OpenVR (phase 7) are not addressed.
