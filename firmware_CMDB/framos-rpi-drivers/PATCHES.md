# Local changes to framos-rpi-drivers

Vendored copy of https://github.com/framosimaging/framos-rpi-drivers
tag `v1.1.1`, branch `framos_20250916`, upstream commit
`c56eef0d232061fa953bccdf514737eeb92e706c`.

Only the tracked sources are kept - no `.git`, no build output. `install-cam.sh`
builds and installs from this directory (and `cam-prebuilt/make-prebuilt.sh`
rebuilds the shipped binaries from it).

## drivers/fr_imx900.c - expose every IMX900 mode under one Bayer order

### Symptom

Without this patch the two highest-resolution modes do not start at all:

```
rp1-cfe 1f00110000.csi: Format mismatch!
rp1-cfe 1f00110000.csi: Failed to start media pipeline: -22
ERROR V4L2 v4l2_videodevice.cpp:2055 /dev/video7[14:out]: Failed to start streaming: Invalid argument
ERROR: *** failed to start camera ***
```

2064x1552 (full sensor) and 1920x1080 fail; only the subsampled modes run, and
those come out with swapped colour channels.

### Why

The driver declares a *different Bayer order per mode* - RGGB for all-pixel and
crop, GBRG for the subsampled ones. libcamera has no such concept: it picks one
native order for the whole sensor, taking the first valid entry from the
numerically sorted mbus code list (`camera_sensor_legacy.cpp`).

Of the six codes the driver advertises, the lowest is
`MEDIA_BUS_FMT_SGBRG10_1X10` (0x300e), so libcamera labels the entire sensor
GBRG and programs the CFE accordingly - while the subdev reports `SRGGB12` for
2064x1552. That mismatch is the `-22`.

The GBRG labels are also simply wrong. Bayer-phase correlation on raw DNGs puts
green on the `(0,1)/(1,0)` antidiagonal in *both* modes - RGGB phase - including
the 1032x776 mode the driver tags GBRG; and the sensor's own test pattern
(`v4l2-ctl -c test_pattern=5`) renders canonically at full resolution but comes
out magenta with checkerboard artefacts at 1032x776. The sensor emits RGGB
throughout; only its description towards libcamera was wrong.

### The change

Each of `modes_12bit` / `modes_10bit` / `modes_8bit` holds five modes, in this
order: all-pixel, crop, subsampling 1/2, subsampling 1/10, binning crop.
Upstream splits them across two codes and truncates both lists:

- `MEDIA_BUS_FMT_SRGGBn` -> `modes_nbit`, `ARRAY_SIZE - 3` => all-pixel + crop
- `MEDIA_BUS_FMT_SGBRGn` -> `&modes_nbit[2]`, `ARRAY_SIZE - 3` => subsampling 1/2 + 1/10

so binning crop (1024x720) is reachable through neither, and the subsampled
modes only under the wrong order.

The patch drops the `SGBRG*` entries from `codes[]` and from `get_mode_table()`,
and lets the `SRGGB*` cases return the full table (`ARRAY_SIZE`, no `- 3`). The
lowest advertised code becomes `SRGGB10_1X10` (0x300f), so libcamera settles on
RGGB, and all five modes are selectable at every bit depth. The mono
(`MEDIA_BUS_FMT_Yn`) paths were already correct and are untouched, as are the
`SGBRG*` cases in the bit-depth switches further down the file, which are pure
depth lookups. `imx900_get_format_code()` needs no change - it only tests
membership of `codes[]` and falls back to `codes[0]`, now `SRGGB12`.

After the patch `rpicam-hello --list-cameras` reports `[2064x1552 12-bit RGGB]`
with five modes under each of `SRGGB8` / `SRGGB10_CSI2P` / `SRGGB12_CSI2P`, and
full resolution captures without any workaround.

## Caveats this patch introduces or leaves in place

**`--hflip` / `--vflip` stop working** (they break the stream with "Failed to
start streaming: Invalid argument"). The flip controls are registered but have
no case in `imx900_set_ctrl()` - they never did anything to the sensor - yet
`vflip` carries `V4L2_CTRL_FLAG_MODIFY_LAYOUT`, so libcamera recomputes the
Bayer order as `GBRG.transform(VFlip)` and asks for a code the driver no longer
advertises. Do not pass `--vflip`/`--hflip`, or `Transform(vflip=1)` in
picamera2. (Before the patch `--vflip` was accidentally a *workaround* for the
full-res failure: a no-op flip that flipped the label back to RGGB.) Removing
`MODIFY_LAYOUT` from the `vflip` control, or unregistering both flips, would
close this off - deliberately not done here, to keep the patch minimal.

**The subsampled modes are not RGGB, and this patch mislabels them.** The claim
that used to stand here - that the phase had been verified for both 2064x1552 and
1032x776, leaving only `2064x154` and `1024x720` inferred - does not survive
measurement. Reading the raw Bayer planes off a DNG of a flat neutral target,
black level 3840 subtracted:

| mode | R/G | B/G | G2/G |
| --- | --- | --- | --- |
| 2064x1552 | 0.538 | 0.405 | 0.997 |
| 1032x776 | 2.164 | 2.153 | 1.382 |

A correct phase puts equal signal in both green planes, which is what full
resolution shows (G2/G 0.997). At 1032x776 they are 38% apart while R and B land
within 0.5% of each other - green is sitting in the R and B slots. The two green
slots hold 582 and 804, reproducing the full-res R:B ratio (804/582 = 1.38
against 1834/1378 = 1.33), which is where R and B went. So the vendor's original
`SGBRG*` labelling was right for the subsampled modes: forcing RGGB across the
whole table fixed the two full-resolution modes and broke the colour of every
subsampled one. Colour gains that neutralise 2064x1552 (G/R 1.02, G/B 1.02)
leave 1032x776 at G/R 0.34, G/B 0.28.

Nothing here works around that yet - IARC2026_raspi_control_flow just defaults
its capture path to the full sensor. Fixing it properly means advertising the
real order per mode again without reintroducing the mode-selection failure the
first half of this patch exists to solve, since libcamera still insists on one
native order for the whole sensor. `2064x154` and `1024x720` remain unmeasured;
given the above, treat them as suspect rather than as RGGB.

## drivers/fr_imx900.c - make VBLANK writable so exposure can be raised

### Symptom

`--framerate` and `--shutter` are both ignored. At 1032x776 the sensor
free-runs at 136.62 fps and `ExposureTime` sticks at 6901 us no matter what is
asked for (72.07 fps at full resolution). Anywhere but bright daylight the AGC
runs out of exposure, pins `AnalogueGain` at its 7.94x ceiling, and the frame
comes out dark and noisy - which then wrecks AWB too, there being no clean
signal left to balance.

### Why

libcamera lengthens a frame *only* through `V4L2_CID_VBLANK`. The driver
registers that control and then pins it:

```c
__v4l2_ctrl_modify_range(imx900->vblank, imx900->min_frame_length_delta,
			 imx900->min_frame_length_delta,
			 1, imx900->min_frame_length_delta);
```

min == max, in both `imx900_adjust_min_frame_length_delta()` and
`imx900_update_frame_rate()`. Exposure is capped at `frame_length -
min_shs_length`, so pinning the frame length pins the exposure ceiling with it.

The vendor's substitute is the custom `frame_rate` control (micro-fps), which
libcamera knows nothing about. It does work, but only when written to the
subdev *after* streaming starts, and the driver resets it to its default on
sensor power-down - so it needs a watchdog to hold it, and libcamera's own AGC
still believes it has no frame-duration headroom.

### The change

Give VBLANK a real range: `min_frame_length_delta` up to the frame length
implied by the mode's own `min_fps` - the slowest rate `frame_rate` already
accepted, so VBLANK cannot reach anywhere `frame_rate` could not
(`imx900_max_frame_length()`).

The range is opened in `imx900_set_limits()`, right after `line_time` becomes
known, rather than left to the `__v4l2_ctrl_s_ctrl(framerate, max_framerate)`
at the end of that function: re-selecting the same mode leaves that control's
value unchanged, and the framework then skips its `s_ctrl` entirely, which
would leave VBLANK pinned. `imx900_update_frame_rate()` opens it as well, so
the `frame_rate` path no longer re-pins what the mode set opened.

VBLANK also has to *do* something. Its `s_ctrl` case only recomputed the
exposure range and never touched the sensor, frame length having been owned by
`frame_rate` alone. It now updates `imx900->frame_length` - which the exposure
range, SHS and the stop-streaming delay all read back - and writes VMAX.

At 1032x776 12-bit VBLANK goes from a pinned 115 to a range of 115..120,952,
which by `exposure_max = vblank + height - min_shs_length` takes the exposure
ceiling from 840 to 121,677 lines - 6.9 ms to ~1 s. `--framerate`, `--shutter`
and libcamera's AGC all work.

### Caveat

`min_fps` is 1 fps for every mode, so VBLANK now advertises a range wide enough
for a one-second frame. That is the vendor's own number, not a measured limit -
nothing here verified that the sensor is happy at its slowest advertised rate.

## Rebuilding

Never hot-reload these modules. `rmmod` + `modprobe` while a CFE pipeline is
live oopses the kernel in `media_gobj_create()` via `cfe_async_complete()` and
taints it `D`. Install and reboot - which is what `install-cam.sh` does.

`make modules_install` prints "missing 'System.map' file. Skipping depmod" and
skips depmod, so it has to be run separately; `install-cam.sh` does that too.
