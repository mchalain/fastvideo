# RP1 front-end configuration file

[← Back to Raspberry Pi](raspberry-pi.md)

On the Raspberry Pi 5, when the camera stream goes through the PiSP Front
End (answer **N** to *disable the pisp_fe* in
[`rpi5cam-setup.sh`](../utils/rpi5cam-setup.sh)), the image on the
`rp1-cfe-fe_image0` node is only produced once the front end has received a
configuration buffer on the `rp1-cfe-fe_config` node.

One configuration per sensor is stored in the [`data/`](../data) directory,
as a **text** (CSV) file named `rp1_fe_config_<sensor>.csv`. It must be
converted with [`csv2pisp_fe_config`](#csv2pisp_fe_config) before being sent
to the driver.

## The configuration node

| Property     | Value                                                  |
|:-------------|:-------------------------------------------------------|
| Entity       | `rp1-cfe-fe_config` (often `/dev/video7`; numbering depends on the board and boot) |
| Buffer type  | `V4L2_BUF_TYPE_META_OUTPUT`                             |
| Format       | `RPFC` (`V4L2_META_FMT_RPI_FE_CFG`)                     |
| Buffer size  | 632 bytes                                              |
| Contents     | one `struct pisp_fe_config`                            |

The structure is defined by the Linux kernel header
`include/uapi/linux/media/raspberrypi/pisp_fe_config.h`; refer to it for the
meaning of each field. At least one buffer must be queued before
`VIDIOC_STREAMON`. On the hardware, the driver then **consumed one config
buffer per frame**, so a producer should keep re-queuing the configuration
even when it does not change (what happens when none is queued has not been
tested).

## CSV format

The file is a plain dump of the binary structure, **one value per byte**:

```
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,23,12,0,2,0,...
```

- **Values** – unsigned integers from 0 to 255, each one being a single
  byte. `csv2pisp_fe_config` accepts decimal (`236`), hexadecimal (`0xec`) or octal
  (`0354`) notation; the repository file uses decimal.
- **Separators** – commas, spaces, tabs and new lines, in any combination.
  Empty fields (`1,,2`) are ignored. The repository file is a single line
  ending with a new line.
- **Order** – the *n*-th value is the byte at offset *n* of the structure,
  starting from 0.
- **Byte order** – multi-byte fields are little-endian, as in the memory of
  the Raspberry Pi: the 16-bit value 2028 (`0x07ec`) is written `236,7`.
- **Length** – exactly 632 values, the size of `struct pisp_fe_config`.
- No header, no comments, no quotes.

## Available configurations

| File                                                              | Sensor | Input            | Output 0                     |
|:------------------------------------------------------------------|:-------|:-----------------|:-----------------------------|
| [`rp1_fe_config_imx477.csv`](../data/rp1_fe_config_imx477.csv)    | imx477 (HQ Camera) | 2028×1520, 16 bits | 2028×1520, compressed 8 bits (`PC1x`) |
| [`rp1_fe_config_ov9281.csv`](../data/rp1_fe_config_ov9281.csv)    | ov9281 | 1280×720, 16 bits | 1280×720, uncompressed 16 bits |

Both enable the same blocks: input, black level (BLA and BLC, 4096 at
16 bits), the RGBY, AGC, AWB and CDAF statistics, and output 0. Lens shading,
defective pixel correction, cropping and downscaling are disabled.

> **Warning:** the sizes must match the sensor format set by
> `rpi5cam-setup.sh` (`Y16_1X16` at 1280×720 for the ov9281). Do not use a
> file with another sensor or another mode.

### imx477

Captured from a running imx477 in its 2028×1520 binned mode. Its output is
compressed (mode 1), which fastvideo cannot read directly. Its dirty flags
only mark some statistics blocks, which suggests it was taken from a frame
after the first one.

### ov9281

Derived from the imx477 file using the field layout of
`pisp_fe_config.h` (kernel 6.18), with these changes:

- input and output 0 set to 1280×720, 16 bits, stride 2560;
- output 0 **uncompressed**, so `rp1-cfe-fe_image0` delivers the same `Y16`
  data as the direct `rp1-cfe-csi2_ch0` path;
- statistics crop and zones sized for 1280×720 (AGC 16×16 zones of 80×45,
  AWB 32×32 zones of 40×23, CDAF 8×8 zones of 160×90);
- Bayer order set to 0: unused with a monochrome sensor, as are the colour
  statistics (AWB, RGBY gains);
- all the enabled blocks, plus the global, floating and output AXI settings,
  marked dirty, so that the driver writes the whole configuration with the
  first buffer.

Tested on a Raspberry Pi 5 with an ov9281 (kernel 6.18.34) using
`application/tests/pisp_fe_test.c`: 60 fps with no dropped frame, real image
data on `rp1-cfe-fe_image0` (black level about 4400, no zero pixels), and one
statistics buffer per frame when the stats link is enabled.

## Sending the configuration with fastvideo

The `config-cam` entry of [`raspi5cam.json`](../data/raspi5cam.json) reads
the binary file with the [file](modules.md#file) module in `static` mode: the
whole file is read again for every buffer, so the front end receives a new
copy of the configuration for each frame. The 632×1 definition with a stride
of 632 gives buffers of exactly 632 bytes.

```json
{
  "name": ["config-cam"],
  "type": "file",
  "path": "./rp1_fe_config.raw",
  "mode": "static",
  "definition": {
    "fourcc": "RPFC",
    "width": 632,
    "height": 1,
    "stride": 632
  },
  "end": true
}
```

The path is relative to the working directory, so install the binary in
`/usr/share/fastvideo` and start fastvideo with `-W`. After
`rpi5cam-setup.sh` (answer **N** to *disable the pisp_fe*), start the
configuration stream **before** the image stream:

```shell
$ csv2pisp_fe_config -o /usr/share/fastvideo/rp1_fe_config.raw data/rp1_fe_config_ov9281.csv
$ fastvideo -W /usr/share/fastvideo -j /etc/fastvideo/raspi5cam.json \
            -i config-cam -o cam-config -i cam-ov9281 -o dryrun
```

To display the image, send it to the GPU (the `gpu` entry renders in an X11
window by default; see the `native` table to use Wayland or DRM). As on the
direct path, the monochrome `Y16 ` format must be overridden,
because it is not a valid DRM fourcc:

```shell
$ fastvideo -W /usr/share/fastvideo -j /etc/fastvideo/raspi5cam.json \
            -i config-cam -o cam-config -i cam-ov9281 -o gpu:fourcc="R16 "
```

Tested with the ov9281: 60 fps and real image data on `rp1-cfe-fe_image0`,
both to `dryrun` and to the GPU display.

## csv2pisp_fe_config

[`csv2pisp_fe_config`](../utils/csv2pisp_fe_config.c) converts a CSV dump into the binary
structure. It is built and installed with the other utilities. Note that
the CSV files themselves are not installed: copy them from the `data/`
directory of the sources.

```shell
$ csv2pisp_fe_config -o rp1_fe_config.raw data/rp1_fe_config_ov9281.csv
```

| Option      | Description                                                 |
|:------------|:------------------------------------------------------------|
| `-o <file>` | Output file (default: standard output).                     |
| `-s <size>` | Expected number of bytes; `0` disables the check (default: 632). |
| `-h`        | Display the help.                                           |
| `INPUT`     | CSV file to read (default: standard input).                 |

The conversion fails, with the offending line number, if a value is not a
number or does not fit into a byte. It also fails if the number of values is
not the expected size, so that a truncated or wrong file is never sent to the
hardware.

The reverse conversion, binary to CSV, can be done with standard tools:

```shell
$ od -An -v -t u1 rp1_fe_config.raw | tr -s ' \n' ',' | sed 's/^,//;s/,$/\n/' > rp1_fe_config_<sensor>.csv
```
