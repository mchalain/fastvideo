# Raspberry Pi

[← Back to README](../README.md)

- [Raspberry Pi 3 and 4](#raspberry-pi-3-and-4)
  - [Camera Module 1 (ov5647)](#camera-module-1-ov5647)
  - [Camera Module 3 (imx708)](#camera-module-3-imx708)
  - [HQ Camera (imx477)](#hq-camera-imx477)
  - [Global Shutter Camera (imx296)](#global-shutter-camera-imx296)
  - [3A algorithms](#3a-algorithms)
- [Raspberry Pi 5](#raspberry-pi-5)
  - [Setting up the media pipeline](#setting-up-the-media-pipeline)
  - [ov9281 camera](#ov9281-camera)
  - [HQ Camera (imx477)](#hq-camera-imx477-1)
  - [Global Shutter Camera (imx296)](#global-shutter-camera-imx296-1)

## Raspberry Pi 3 and 4

The project provides configuration files for the Raspberry Pi and its
Broadcom ISP. The [main configuration file](../data/raspicam.json) contains
the default settings for the camera, ISP, GPU and screen. Each camera module
also needs its own settings file (`setting-<sensor>.json`).

By default, the output is rendered by the GPU in an X11 window. The available
native back ends are `x11`, `wayland`, `drm` and `offscreen`; the one used is
the first entry of the `native` table of the `gpu` object.

In each example below, the first command starts the stream and the second
applies the sensor settings.

### Camera Module 1 (ov5647)

```shell
$ fastvideo -D -j /etc/fastvideo/raspicam.json -i cam-ov5647 -o isp-in -i isp-out -o gpu
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-ov5647.json
```

### Camera Module 3 (imx708)

This sensor must stream the image and its embedded metadata at the same time:

```shell
$ fastvideo -D -j /etc/fastvideo/raspicam.json -i cam-imx708 -o isp-in -i unicam-embedded -o dryrun -i isp-out -o gpu
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-imx708.json
```

### HQ Camera (imx477)

This sensor must stream the image and its embedded metadata at the same time:

```shell
$ fastvideo -D -j /etc/fastvideo/raspicam.json -i cam-imx477 -o isp-in -i unicam-embedded -o dryrun -i isp-out -o gpu
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-imx477.json
```

### Global Shutter Camera (imx296)

```shell
$ fastvideo -D -j /etc/fastvideo/raspicam.json -i cam-imx296 -o isp-in -i isp-out -o gpu
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-imx296.json
```

### 3A algorithms

A 3A server named **rpivc4_alg** is available in the [`utils/`](../utils)
directory. It receives statistics from a `fastvideo` FIFO and sends controls
to the `fastsetting` socket.

```shell
$ rpivc4_alg -D
$ fastvideo -D -j /etc/fastvideo/raspicam.json -i isp-meta -o rpivc4_alg
```

The complete system looks like this:

```
 camera image      --> fastvideo #1 --> isp-in --> isp-out --> fastvideo #1 --> gpu
 camera embedded   --> fastvideo #1 --> dryrun           (sensors with metadata only)
 camera statistics --> fastvideo #2 --> FIFO --> rpivc4_alg
 rpivc4_alg        <-> UNIX socket  <-> fastsetting
```

> **Note:** because `rpivc4_alg` reads from a FIFO, both processes are
> synchronised: if one is restarted, the other must be restarted too.

> **Note:** `rpivc4_alg` currently implements only an auto-exposure (AE)
> algorithm.

## Raspberry Pi 5

The Raspberry Pi 5 uses a new chip, the RP1, whose camera front end needs a
stream of configuration data to initialise and run. Fastvideo can already
set up the RP1, but the generator for this data is still missing.

Fastvideo can therefore use the camera in raw mode. With a Bayer sensor, the
raw image displayed as monochrome is blurred: either use a monochrome camera,
or let the GPU act as the ISP with the
[`debayer.glsl`](../data/debayer.glsl) program, as shown in
[`raspi5cam.json`](../data/raspi5cam.json).

### Setting up the media pipeline

The media pipeline inside the RP1 front end must be configured before
streaming. The [`rpi5cam-setup.sh`](../utils/rpi5cam-setup.sh) tool helps
with this:

```
$ ./utils/rpi5cam-setup.sh auto
Media : /dev/media0
current 2028x1520 change (y/N)?
current fmt SRGGB12_1X12 of imx477 10-001a (16). Change it (y/N)?
current fmt SRGGB16_1X16 of pisp-fe. Change it (y/N):
disable the pisp_fe (y/N)y
fmt image SRGGB16_1X16
subdev format code 0x3020/0x3012 SRGGB16_1X16 => output fourcc RG16
Change fourcc ? [y/N]
0: media topology
1: Image device configuration
2: try to stream into file
3: take a picture
your choice ?
Device set to /dev/video4
```

> **Important:** you **must** answer **y** to the question
> *disable the pisp_fe*. Otherwise the image goes through the PiSP Front
> End, which produces nothing until it receives a configuration buffer: see
> [RP1 front-end configuration file](rp1-fe-config.md).

### ov9281 camera

The ov9281 is a monochrome global shutter sensor. Its output is Y10,
1920×1080 at 120 fps.

```shell
$ fastvideo -D -j /etc/fastvideo/raspi5cam.json -i raw-ov9281 -o gpu:fourcc="R16 "
$ fastsetting -j /etc/fastvideo/raspi5cam.json -J /etc/fastvideo/setting-ov9281.json
```

### HQ Camera (imx477)

```shell
$ fastvideo -D -j /etc/fastvideo/raspi5cam.json -i raw-imx477 -o gpu-isp
$ fastsetting -j /etc/fastvideo/raspi5cam.json -J /etc/fastvideo/setting-imx477.json
```

### Global Shutter Camera (imx296)

```shell
$ fastvideo -D -j /etc/fastvideo/raspi5cam.json -i raw-imx296 -t toR16 -o gpu-isp
$ fastsetting -j /etc/fastvideo/raspi5cam.json -J /etc/fastvideo/setting-imx296.json
```
