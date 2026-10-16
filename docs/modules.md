# Modules

[← Back to README](../README.md)

Every stage of a stream is a module, described by an object in the JSON
configuration file. This page lists the available modules and the JSON
entries they accept.

- [Feature matrix](#feature-matrix)
- [Common entries](#common-entries)
- [v4l2](#v4l2) – cameras, ISPs, encoders and decoders
- [subv4l](#subv4l) – V4L2 sub-device control
- [gpu](#gpu) – OpenGL ES rendering and processing
- [screen](#screen) – DRM/KMS display
- [mpegts](#mpegts) – MPEG-2 transport stream output
- [passthrough](#passthrough) – buffer forwarding, tee, dry run and conversion
- [file](#file) – file, FIFO and socket input/output

## Feature matrix

| Module                      | Variant | Source | Sink | Transfer | Control | dma_fd | HW memory | SW memory | Comment                         |
|:----------------------------|:--------|:------:|:----:|:--------:|:-------:|:------:|:---------:|:---------:|:--------------------------------|
| [v4l2](#v4l2)               | capture | ✓      |      |          | ✓       | ✓      | ✓         |           | Camera, ISP…                    |
|                             | m2m     |        |      | ✓        | ✓       | ✓      | ✓         |           | ISP, encoder/decoder…           |
|                             | output  |        | ✓    |          | ✓       | ✓      | ✓         |           | ISP…                            |
| [subv4l](#subv4l)           |         |        |      |          | ✓       |        |           |           | Controls V4L2 media only        |
| [screen](#screen)           |         |        | ✓    | ✓        | ✓       | ✓      | ✓         |           | HDMI, writeback                 |
| [gpu](#gpu)                 | drm     |        | ✓    | ✓        |         | ✓      | ✓         | ✓         | Pixel or Mesa export            |
|                             | x11     |        | ✓    | ✓        |         | ✓      | ✓         | ✓         |                                 |
|                             | wayland |        | ✓    | ✓        |         | ✓      | ✓         | ✓         |                                 |
| [mpegts](#mpegts)           | udp     |        | ✓    |          |         |        |           | ✓         | Can be played with `ffplay`     |
|                             | unix    |        | ✓    |          |         |        |           | ✓         | Needs a local client            |
|                             | file    |        | ✓    |          |         |        |           | ✓         | Generates HLS files             |
| [file](#file)               |         | ✓      | ✓    |          |         |        |           | ✓         | Copies data from/to a file      |
| [passthrough](#passthrough) |         |        |      | ✓        |         | ✓      | ✓         | ✓         | Simple buffer transfer          |
|                             | dryrun  |        | ✓    |          |         | ✓      | ✓         | ✓         | Discards buffers (for testing)  |
|                             | tee     |        |      | ✓        |         | ✓      | ✓         | ✓         | Sends buffers to two sinks      |
|                             | convert |        |      | ✓        |         |        |           | ✓         | Applies a CPU filter to images  |

## Common entries

Most modules share the following entries:

| Entry        | Type                 | Parent                    | Description                                    |
|:-------------|:--------------------:|:--------------------------|:-----------------------------------------------|
| `type`       | string               | root                      | Module type.                                   |
| `name`       | string &#124; array  | root                      | One or several names for the module.           |
| `device`     | string               | root                      | Path of the character device.                  |
| `definition` | object               | root                      | Input (source) format.                         |
| `transfer`   | object               | root                      | Output format, when used as a transfer.        |
| `fourcc`     | string               | `definition`, `transfer`  | Pixel format.                                  |
| `modifier`   | integer              | `definition`, `transfer`  | Pixel format modifier.                         |
| `width`      | integer              | `definition`, `transfer`  | Image width.                                   |
| `height`     | integer              | `definition`, `transfer`  | Image height.                                  |
| `stride`     | integer              | `definition`, `transfer`  | Image pitch (bytes per line).                  |

## v4l2

Drives V4L2 capture, memory-to-memory and output devices.

This module does not yet support the newer multi-stream media devices. The
media device linked to the V4L2 device must be configured beforehand with
`media-ctl`; helper scripts are provided for Raspberry Pi boards (see
[Raspberry Pi](raspberry-pi.md)).

```json
{
  "name": ["isp1-in", "bcm2835-isp0-output0"],
  "type": "v4l2",
  "device": "/dev/video20",
  "definition": {},
  "transformation": [],
  "controls": [
    {
      "name": "Red Balance",
      "id": 9963790,
      "value": 1000
    }
  ]
}
```

In addition to the [common entries](#common-entries) (`type` must be
`"v4l2"`):

| Entry      | Type             | Parent     | Description                      |
|:-----------|:----------------:|:-----------|:---------------------------------|
| `fps`      | integer          | root       | Frame rate of the stream.        |
| `controls` | array of objects | root       | Image settings.                  |
| `name`     | string           | `controls` | Name of the setting.             |
| `id`       | integer          | `controls` | V4L2 control ID of the setting.  |
| `value`    | integer          | `controls` | Value of the setting.            |

## subv4l

Modern camera sensor drivers often separate the data stream from the
settings of the sensor, focus lens or lighting. The `subv4l` modules control
these sub-devices: they are used by [`fastsetting`](applications.md#fastsetting)
and never produce a stream.

A `subv4l` object may be placed at the root of the JSON file or inside the
`subdevices` array of a `v4l2` object.

```json
{
  "name": ["cam-ov5647", "ov5647 10-0036"],
  "disable": true,
  "type": "subv4l",
  "device": "/dev/v4l-subdev0",
  "definition": {
    "fourcc": "GB10",
    "width": 1920,
    "height": 1080,
    "fps": 30
  }
}
```

The entries are the same as for the [v4l2](#v4l2) module.

## gpu

Uses OpenGL ES and EGL to process images and display them. The native
back end may be `drm`, `x11`, `wayland` or `offscreen`.

The module can run GLSL programs on the images and export the result from
the GPU in two ways:

- **pixels** – uses `glReadPixels()`; slow, but always available;
- **imagemesa** – uses `eglExportDMABUFImageMESA()`; very fast, but the
  output may be altered by the GPU's tiled memory layout.

```json
{
  "name": ["gpu"],
  "type": "gpu",
  "native": "drm",
  "device": "/dev/dri/card0"
}
```

In addition to the [common entries](#common-entries) (`type` must be
`"gpu"`):

| Entry      | Type             | Parent     | Description                                         |
|:-----------|:----------------:|:-----------|:----------------------------------------------------|
| `native`   | string           | root       | Native API: `"drm"`, `"x11"`, `"wayland"` or `"offscreen"`. |
| `device`   | string           | root       | Character device path (`drm` only).                 |
| `programs` | array of objects | root       | GLSL programs to apply.                             |
| `vertex`   | string           | `programs` | Path of the GLSL vertex shader.                     |
| `fragment` | string           | `programs` | Path of the GLSL fragment shader.                   |
| `export`   | string           | root       | Export API: `"pixels"` or `"imagemesa"`.            |

## screen

Uses the DRM device to display images on the screen. It can also use the
writeback connector to transfer images, and can control image rotation.

```json
{
  "name": ["screen"],
  "type": "screen",
  "device": "/dev/dri/card0",
  "controls": [
    {
      "name": "rotation",
      "value": "reflect"
    }
  ]
}
```

In addition to the [common entries](#common-entries) (`type` must be
`"screen"`):

| Entry      | Type                  | Parent     | Description                                  |
|:-----------|:---------------------:|:-----------|:---------------------------------------------|
| `controls` | array of objects      | root       | Image settings.                              |
| `name`     | string                | `controls` | Name of the setting; must be `"rotation"`.   |
| `value`    | string &#124; integer | `controls` | `"reflect"`, `"90"`, `"180"` or `"270"`.     |

## mpegts

Generates an MPEG-2 transport stream. The input must be an H.264 stream.

```json
{
  "name": ["dvb-i"],
  "type": "mpegts",
  "proto": "udp",
  "host": "192.168.1.10",
  "port": 5024,
  "periodic": 60
}
```

| Entry      | Type                 | Parent | Description                                          |
|:-----------|:--------------------:|:-------|:-----------------------------------------------------|
| `type`     | string               | root   | Must be `"mpegts"`.                                  |
| `name`     | string &#124; array  | root   | One or several names for the module.                 |
| `proto`    | string               | root   | `"udp"`, `"unix"`, `"file"`, `"fifo"` or `"tcp"`.    |
| `host`     | string               | root   | Destination address or socket path.                  |
| `port`     | integer              | root   | Port number (`udp` only).                            |
| `periodic` | integer              | root   | Number of frames between two I-frames.               |

## passthrough

By default, this module simply hands buffers from one module to the next.
Some options change its behaviour:

- **dryrun** discards the stream: the module becomes a sink;
- **tee** sends the stream to two sinks;
- **copy** dumps the stream before passing it on;
- **convert** applies a filter function provided by another library (see
  [Writing a convert library](convert-library.md)).

```json
{
  "name": "untile",
  "type": "passthrough",
  "convert": "untile",
  "library": "/usr/lib/fastvideo/libconvertuntile.so"
}
```

| Entry      | Type                  | Parent     | Description                                       |
|:-----------|:---------------------:|:-----------|:--------------------------------------------------|
| `type`     | string                | root       | Must be `"passthrough"`.                          |
| `name`     | string &#124; array   | root       | One or several names for the module.              |
| `branch`   | object                | root       | Description of the second sink (tee).             |
| `name`     | string                | `branch`   | Name of the module.                               |
| `type`     | string                | `branch`   | Type of the module: `"v4l2"`, `"gpu"`, `"screen"`.|
| `convert`  | string &#124; object  | root       | Converter to use (see [convert library](convert-library.md)). |
| `controls` | array of objects      | root       | Settings.                                         |
| `dryrun`   | boolean               | `controls` | Discard the stream.                               |
| `tee`      | boolean               | `controls` | Enable the `branch` sink.                         |

## file

Writes data to, or reads data from, a file, a FIFO or a socket. If the path
is a directory, a `stream_XXX` file is created inside it.

The path can also be given on the command line:

```shell
$ fastvideo -j fastconfig.json -i cam -o file:./test.mjpeg
```

```json
{
  "name": "file",
  "type": "file"
}
```

| Entry  | Type                 | Parent | Description                                        |
|:-------|:--------------------:|:-------|:---------------------------------------------------|
| `type` | string               | root   | Must be `"file"`.                                  |
| `name` | string &#124; array  | root   | One or several names for the module.               |
| `path` | string               | root   | File or directory to use.                          |
| `mode` | string &#124; array  | root   | `"file"`, `"fifo"`, `"tcp"`, `"udp"` or `"unix"`.  |
