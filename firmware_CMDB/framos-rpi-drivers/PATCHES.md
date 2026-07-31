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

**Two mode phases are inferred, not measured.** The Bayer phase was verified for
2064x1552 and 1032x776. `2064x154` and `1024x720` are now exposed as RGGB on the
same reasoning but were never checked against a test pattern.

**`--framerate` does not work** - unrelated to this patch. The driver pins
`vertical_blanking` to min=max=137, which is the only lever libcamera has on
frame duration, so the sensor free-runs (72.07 fps at full resolution). The
vendor `frame_rate` V4L2 control (micro-fps, set on the subdev *after* the
stream starts) is the only working limit, and libcamera resets it on every
configure - so it needs a watchdog to hold.

## Rebuilding

Never hot-reload these modules. `rmmod` + `modprobe` while a CFE pipeline is
live oopses the kernel in `media_gobj_create()` via `cfe_async_complete()` and
taints it `D`. Install and reboot - which is what `install-cam.sh` does.

`make modules_install` prints "missing 'System.map' file. Skipping depmod" and
skips depmod, so it has to be run separately; `install-cam.sh` does that too.
