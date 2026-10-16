# Fastvideo

**A lightweight, zero-copy video pipeline framework for embedded Linux.**

Fastvideo is a C library (`libfastvideo.so`) and a small set of command-line
applications for building real-time video streams from cameras, ISPs, GPUs,
displays and network outputs. It is designed for small boards such as the
Raspberry Pi, where every copy of a frame through user space costs precious
CPU time.

## Why Fastvideo?

- **Zero copy wherever possible** – frames travel from one device to the next
  as DMA buffers (`dma_fd`), so the CPU never has to touch the pixels.
- **Modular by design** – every stage of a stream (camera, ISP, GPU,
  screen, encoder, network, file…) is a plugin. A stream is simply a chain
  of plugins described on the command line.
- **Declarative configuration** – hardware is described once in a JSON
  file; the command line only picks which devices to connect.
- **GPU processing** – apply GLSL shaders (including
  [ShaderToy](https://www.shadertoy.com) filters) to live video with
  OpenGL ES, on X11, Wayland, DRM/KMS or off-screen.
- **Live control** – camera and ISP settings can be changed while streaming,
  through a UNIX socket, which makes Fastvideo easy to drive from a web server.

## How it works

A stream is built from **sources** (`-i`), optional **transfers** (`-t`) and
**sinks** (`-o`), each named after an entry in the JSON configuration file:

```
  -i cam-imx477      -o isp-in        -i isp-out        -o gpu
 ┌─────────────┐   ┌──────────┐     ┌───────────┐     ┌──────────┐
 │   camera    │──▶│ ISP in   │ ... │  ISP out  │────▶│   GPU    │──▶ screen
 └─────────────┘   └──────────┘     └───────────┘     └──────────┘
          dma_fd buffers, no copy through user space
```

The project provides three applications:

| Application                                      | Purpose                                                        |
|:-------------------------------------------------|:---------------------------------------------------------------|
| [`fastvideo`](docs/applications.md#fastvideo)     | Creates and runs the streams described by a JSON file.         |
| [`fastconfig`](docs/applications.md#fastconfig)   | Generates a JSON file listing all the hardware found on the host. |
| [`fastsetting`](docs/applications.md#fastsetting) | Opens a UNIX socket that receives JSON objects to control the devices. |

Image-quality algorithms (AWB, AEC, AF) are hardware-specific, so they run in
a separate server: Fastvideo extracts statistics from the V4L2 metadata
capture device and pushes them to that server through a named pipe. These
servers live in the [`utils/`](utils) directory.

## Quick start

On a Debian-like system with a USB webcam:

```shell
$ sudo apt install libjansson-dev wayland-protocols libdrm-dev libgbm-dev \
                   libgles2-mesa-dev libegl1-mesa-dev
$ make defconfig
$ make
$ sudo make install
$ fastvideo -j /etc/fastvideo/uvc_desktop.json -i uvc -o gpu
```

The webcam image should appear in a window rendered by the GPU.

## Documentation

| Guide                                                | Contents                                                          |
|:-----------------------------------------------------|:------------------------------------------------------------------|
| [Building and installing](docs/building.md)          | Dependencies, native and cross compilation, installation paths.   |
| [Applications](docs/applications.md)                 | Command-line reference for `fastvideo`, `fastconfig` and `fastsetting`. |
| [Modules](docs/modules.md)                           | Feature matrix and JSON reference for every module (`v4l2`, `gpu`, `screen`, …). |
| [Examples on a PC](docs/examples-pc.md)              | Webcam, ShaderToy filters, software trigger, performance checks.  |
| [Raspberry Pi](docs/raspberry-pi.md)                 | Camera modules on Raspberry Pi 3, 4 and 5, 3A algorithms.          |
| [Writing a convert library](docs/convert-library.md) | Adding your own CPU pixel converters.                             |
| [Contributing and roadmap](docs/contributing.md)     | Writing a new module and the features still missing.             |

## Licence

Fastvideo is released under the [MIT Licence](LICENSE).
