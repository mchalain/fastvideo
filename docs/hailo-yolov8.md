# Object detection with Hailo and YOLOv8

[← Back to README](../README.md)

Fastvideo can run a YOLO object detector on a Hailo-8 or Hailo-8L AI
accelerator and draw the detections over the live video, with almost no CPU
work: the camera image goes from the ISP to the accelerator as a DMA buffer,
the detections come back as a small NMS buffer, and the boxes are drawn by the
GPU shader that displays the video.

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [The YOLOv8 model](#the-yolov8-model)
- [Building](#building)
- [Configuration](#configuration)
- [Running](#running)
- [The hailo module](#the-hailo-module)
- [nmsclient](#nmsclient)
- [Drawing rectangles: rectangles.glsl and rectdraw](#drawing-rectangles-rectanglesglsl-and-rectdraw)
- [Performance](#performance)
- [Troubleshooting](#troubleshooting)

## How it works

```
                      ┌─► isp-out  (1280×720 XB24) ──► gpu (rectangles.glsl) ──► screen
 camera ──► isp-in ───┤                                   ▲
                      │                                   └────────────────────────┐
                      │                                   vRects[] (shared memory) │
                      └─► isp-out2 (640×640 YUYV) ──► hailo (yolov8) ──► fifo ──► nmsclient
```

1. The ISP produces two images from each camera frame: one for the display,
   and a 640×640 YUYV one for the model.
2. The [hailo](#the-hailo-module) module (`libfasthailo.so`) sends the YUYV
   image to the accelerator. The model converts it to RGB on the chip, runs
   YOLOv8n and its NMS post-processing, and returns one buffer of detections
   per image.
3. A [file](modules.md#file) module in FIFO mode writes these buffers into a
   named pipe.
4. [`nmsclient`](#nmsclient) reads the pipe, keeps the best detections and
   writes them as rectangles into the shared memory of the GPU program.
5. The [`rectangles.glsl`](#drawing-rectangles-rectanglesglsl-and-rectdraw)
   fragment shader draws the rectangles over the displayed image at the next
   frame.

## Requirements

| Item | Version used | Note |
|:-----|:-------------|:-----|
| Accelerator | Hailo-8L (M.2) | Hailo-8 works the same way |
| HailoRT driver and firmware | 4.24.0 | must match the library version |
| HailoRT library (`libhailort.so`) | 4.24.0 | the C API (`hailo/hailort.h`) is used |
| Model | Yolov8n_yuy2 | [Yolov8n optimized for yuy2 input image](https://github.com/mchalain/fastvideo/releases/download/Ai_models/yolov8n_yuy2.hef) |
| Board | Raspberry Pi CM4 | any board (to test on Pi 5) with a V4L2 source able to give YUYV |
| Camera | ov9281 (monochrome) | any kind of sensor through the Broadcom ISP |

The HailoRT driver (`/dev/hailo0`) and the library must have exactly the same
version.

## The YOLOv8 model

The model used is a YOLOv8n compiled for the Hailo-8L with a YUY2 input:

```shell
$ wget https://github.com/mchalain/fastvideo/releases/download/Ai_models/yolov8n_yuy2.hef
$ cp yolov8n_yuy2.hef /media/models/yolov8n.hef
```

| Stream | Format | Shape | Frame size |
|:-------|:-------|:------|:-----------|
| input `yolov8n/input_layer1` | UINT8, YUY2 | 640×640×2 | 819 200 bytes |
| output `yolov8n/yolov8_nms_postprocess` | FLOAT32, NMS by class | 80 classes, 100 boxes per class | 160 320 bytes |

Why YUY2: the Raspberry Pi 4 ISP gives RGB only on its main output (used for
the display); its second output only produces YUV formats. The model was
therefore compiled with a `yuy2_to_rgb` input translator, so that the
accelerator converts the image itself and the ISP output can be sent as it
is, without CPU conversion.

Two things to check before using another HEF (`hailortcli parse-hef <file>`
shows them):

- **One input stream.** The hailo module writes a single raw input stream. A
  model compiled with an NV12 input, for example, has two raw input streams
  (Y and UV planes) and is refused.
- **NMS on the chip.** The output must be an NMS stream (`HAILO_NMS_BY_CLASS`
  or `HAILO_NMS_BY_SCORE`). A model without NMS post-processing (e.g. the
  `yolov6n_0.2.1.hef` of the Hailo model zoo) gives raw convolution outputs
  that `nmsclient` cannot read.

## Building

The hailo module is built into its own library, `libfasthailo.so`, when the
HailoRT headers and library are found (`HAVE_HAILORT`, see
[`src/libfastvideo.mk`](../src/libfastvideo.mk)). `nmsclient` and `rectdraw`
need [Jansson](https://github.com/akheron/jansson).

Cross compilation with a Buildroot SDK (see [Building](building.md)):

```shell
$ export PATH=/opt/aarch64-buildroot-linux-gnu_sdk-buildroot/bin:$PATH
$ make BUILDDIR=$PWD/build-aarch64 CROSS_COMPILE=aarch64-buildroot-linux-gnu \
       SYSROOT=/opt/aarch64-buildroot-linux-gnu_sdk-buildroot/aarch64-buildroot-linux-gnu/sysroot \
       prefix=/usr sysconfdir=/etc/fastvideo defconfig
$ cd build-aarch64
$ make
```

It produces `src/libfasthailo.so`, `utils/nmsclient` and `utils/rectdraw`.
When `SYSROOT` is set, makemore gives the sysroot to pkg-config itself, so the
`.pc` files of the sysroot never produce host paths such as `-L/usr/lib`.

## Configuration

Four entries of the JSON configuration are involved
([`data/raspicam.json`](../data/raspicam.json)).

**The model input**: the second ISP output, at the size and format of the
model input.

```json
{
  "name": ["isp-out2", "bcm2835-isp0-capture2"],
  "type": "v4l2",
  "device": "/dev/video15",
  "definition": { "fourcc": "YUYV", "width": 640, "height": 640, "end": true },
  "end": true
}
```

**The model**:

```json
{ "name": ["yolov8"], "type": "hailo", "hef": "/media/models/yolov8n.hef", "end": true }
```

**The detections output**: a named pipe. The protocol is selected with
`"proto"`; `"mode"` only carries the protocol options.

```json
{ "name": ["nms-fifo"], "type": "file", "path": "/tmp/nms.fifo", "proto": "fifo", "end": true }
```

**The display**: the `gpu` device runs the
[`rectangles.glsl`](#drawing-rectangles-rectanglesglsl-and-rectdraw) program,
with its ten `vRects` controls declared (set to zero).

## Running

```shell
$ fastvideo -l libfasthailo.so -W /etc/fastvideo/ -j /etc/fastvideo/raspicam.json \
            -i cam-ov9281:fps=120 -o isp-in \
            -i isp-out -o gpu \
            -i isp-out2 -t yolov8 -o nms-fifo &
$ sleep 2
$ nmsclient -f /tmp/nms.fifo
```

- **Start fastvideo first.** `nmsclient` attaches to the shared memory that
  fastvideo creates for the GPU program.
- **Start `nmsclient` quickly.** fastvideo opens the FIFO in read-write mode,
  so it never waits for a reader, but its writes block as soon as the pipe is
  full: one NMS buffer (160 KB) is already larger than a pipe. Until
  `nmsclient` reads, the whole pipeline is stopped.
- **Stop fastvideo first**, then `nmsclient` (which clears its rectangles on
  exit). In the other order, fastvideo blocks for ever in its FIFO write and
  only `SIGKILL` stops it.

## The hailo module

[`src/shailo.c`](../src/shailo.c), loaded with `-l libfasthailo.so`, registers
the `hailo` device type. It is a transfer module (`-t`): the image goes in, a
buffer of detections comes out.

| Entry      | Type    | Description |
|:-----------|:-------:|:------------|
| `type`     | string  | must be `"hailo"` |
| `hef`      | string  | path of the HEF file |
| `device`   | string  | HailoRT device id (e.g. `0000:01:00.0`); by default the first device found |
| `nbuffers` | integer | number of buffers on each side (default 12); `-t yolov8:nbuffers=N` on the command line takes precedence |

- The input is written with the asynchronous raw stream API
  (`hailo_stream_write_raw_buffer_async()`), from the buffers of the upstream
  module: no copy. The input buffer size must be the frame size of the HEF
  input stream, otherwise `shailo: framesize mismatch` is printed.
- The output is the raw buffer of the output vstream, in HailoRT's default
  format (`HAILO_FORMAT_TYPE_AUTO`), its fourcc is `NMS `. It is always the
  worst-case size (160 320 bytes for YOLOv8n with 80 × 100 boxes), without any
  header.
- Only models with a single input stream and a single output vstream are
  supported.

## nmsclient

[`utils/nmsclient.c`](../utils/nmsclient.c) reads the NMS buffers from the
FIFO, keeps the detections above a score, sorts them by score, and writes the
best ones into the `vRects` slots of the GPU program (clearing the others).
On exit it clears all its rectangles.

```shell
$ nmsclient -f /tmp/nms.fifo [-s 0.5] [-k 0] [-v]
```

| Option        | Description | Default |
|:--------------|:------------|:--------|
| `-f <fifo>`   | FIFO written by fastvideo (created if missing) | required |
| `-m <layout>` | NMS layout: `byclass` or `byscore` | `byclass` |
| `-c <n>`      | number of classes of the model | 80 |
| `-b <n>`      | max boxes per class (`byclass`) | 100 |
| `-S <size>`   | frame size in bytes | computed from `-m`, `-c`, `-b` |
| `-s <score>`  | minimum score | 0.5 |
| `-k <class>`  | keep only this class (0 = person in COCO) | all |
| `-r <x,y,w,h>`| part of the displayed image seen by the model | `0,0,1,1` |
| `-N <n>`      | max rectangles drawn | all the `vRects` slots |
| `-n`          | do not draw (with `-v`) | |
| `-v`          | print the detections of each frame as a JSON line | |
| `-j <json>`   | fastvideo configuration file, to find the layout of the shared memory | `/etc/fastvideo/raspicam.json` |
| `-d <device>` | name of the gpu device into the configuration | `gpu` |
| `-p <program>`| name of the GLSL program of this device | `rectangles` |
| `-W <path>`   | directory of the shared memory files of the GPU programs | `/tmp/fastvideo.segl/` |
| `-D`          | run as a daemon | |
| `-L <file>`   | log file | |
| `-P <file>`   | PID file (daemon mode) | |

The two NMS layouts of HailoRT are supported:

- **by class** (`HAILO_FORMAT_ORDER_HAILO_NMS_BY_CLASS`, the default of NMS
  output vstreams): for each class, a `float32` count followed by `count`
  boxes (`y_min, x_min, y_max, x_max, score`, 20 bytes). There is no
  terminator: the reader stops after `-c` classes.
- **by score** (`HAILO_FORMAT_ORDER_HAILO_NMS_BY_SCORE`): a `uint16` count
  followed by `count` detections (22 bytes, with the class id).

The HailoRT structures are packed; `nmsclient` uses its own copies and does
not depend on HailoRT.

The coordinates are normalised to the model input. When the model sees only a
part of the displayed image (crop), `-r` maps them back: for a model seeing
the left half of the image, use `-r 0,0,0.5,1`. When the whole image is scaled
to the model input, as here, the default is right.

With `-v`, each frame gives one line:

```json
{"frame":285,"detections":[{"class":0,"score":0.881,"x1":0.1954,"y1":0.3040,"x2":0.4880,"y2":0.6629}]}
```

## Drawing rectangles: rectangles.glsl and rectdraw

[`data/rectangles.glsl`](../data/rectangles.glsl) is a fragment shader that
draws the outlines of up to 10 rectangles over the image.

| Uniform | Type | Description |
|:--------|:-----|:------------|
| `vRects[0]` … `vRects[9]` | vec4 | `(x1, y1, x2, y2)` in normalised image coordinates; a rectangle with `x2 ≤ x1` or `y2 ≤ y1` (e.g. all zeros) is not drawn |
| `vColor` | vec4 | outline colour (default opaque red); its alpha blends the outline with the image |
| `vThickness` | float | outline width in pixels (default 2) |
| `vMono` | int | `1`: show the red channel as grey, for single-channel sources (R8/R16); `0` for RGB/XB24 sources |

```json
{
  "name": "rectangles",
  "vertex": "/usr/share/fastvideo/simplevertex.glsl",
  "fragment": "/usr/share/fastvideo/rectangles.glsl",
  "controls": [
    { "name": "vRects[0]", "type": "vec4", "value": [0.0, 0.0, 0.0, 0.0] },
    …
    { "name": "vRects[9]", "type": "vec4", "value": [0.0, 0.0, 0.0, 0.0] },
    { "name": "vColor", "type": "vec4", "value": [1.0, 0.0, 0.0, 1.0] },
    { "name": "vThickness", "type": "float", "value": 4.0 },
    { "name": "vMono", "type": "int", "value": 0 }
  ],
  "end": true
}
```

- The `vec4` and `float` values must be written with a decimal point
  (`0.0`, not `0`): they are read with `json_real_value()`, which returns 0
  for a JSON integer.
- All the `vRects` must be declared: only the declared controls own a slot in
  the shared memory.

**Shared memory.** The values of the controls of each GPU program live in a
System V shared memory segment, created by fastvideo with the key
`ftok("/tmp/fastvideo.segl/<program name>", 'R')`, and re-read by the GPU at
every frame. The layout is not stored anywhere: the controls with a fixed size
are packed in the **reverse** order of the JSON array. `nmsclient` and
`rectdraw` read the same JSON to compute it (see
[`utils/rectshm.c`](../utils/rectshm.c)).

**rectdraw** sets rectangles by hand, for tests or for another application:

```shell
$ rectdraw -a 0.35,0.3,0.3,0.4   # x,y,w,h into the first empty slot
$ rectdraw -r                     # clear the last rectangle in use
$ rectdraw -c                     # clear all
$ rectdraw -l                     # list the slots
```

Note that `nmsclient` rewrites all its slots at every frame: use `-N` to leave
some slots to `rectdraw`.

## Performance

Measured on a Raspberry Pi CM4 with a Hailo-8L (M.2, PCIe Gen2), ov9281 at
120 fps, display at 1280×720:

- the display runs at the screen refresh rate (60 Hz): it does not depend on
  the detection;
- the detection runs at about **50 inferences per second**;
- with a test image of two people, the two persons are detected (scores 0.88
  and 0.56) at the expected positions, at every frame.

### The bottleneck: a multi-context model

The 50 inferences per second are the limit of the accelerator with this
model, not of fastvideo. A small HailoRT benchmark, with the same calls as
the hailo module, gives:

| Frames in flight | Batch size | Inferences per second |
|:-----------------|:-----------|:----------------------|
| 1 (send, wait for the result) | 1 | 45.0 |
| 2, 4 or 8 | 1 | 49.6 |
| 16 | 2 | 65.0 |
| 16 | 4 | 77.3 |
| 16 | 8 | 85.8 |

Sending more frames at a time barely helps: the device needs about 20 ms per
frame. Two reasons:

- **The model is multi-context.** This YOLOv8n, with its `yuy2_to_rgb` input
  translator, does not fit into the Hailo-8L at once: the compiler split it
  into several contexts, and the device reloads them for every frame. The
  `yolov6n` HEF of the model zoo, for comparison, has a single context.
  `hailortcli parse-hef <file>` shows it.
- **The PCIe link of the CM4 is Gen 2 ×1** (5 GT/s,
  `/sys/bus/pci/devices/0000:01:00.0/current_link_speed`), half the
  bandwidth of a Raspberry Pi 5 in Gen 3. The context reloads go through it.

The temperature does not play any part.

A **batch** shares the reload of the contexts between several frames: the
throughput rises (85.8 fps for a batch of 8), but the result of a frame waits
for the other frames of its batch, and the stream needs at least as many
buffers as the batch size.

### Going faster

1. **A single-context model**: compile YOLOv8n so that it fits into one
   context (check whether the input translator is what makes it
   multi-context, by comparing with the precompiled HEF of the model zoo).
2. **A batch larger than 1**: the `st-hailo` module of the application has a
   `batch` option (`"batch": 4` in the JSON, or `-t <name>:batch=4`), applied
   to the network group when it is configured. The batch must not be larger
   than the number of buffers (`nbuffers`).
3. **A PCIe Gen 3 host**, such as a Raspberry Pi 5.

## Troubleshooting

| Message or symptom | Cause |
|:-------------------|:------|
| `shailo: ... unknown buffer type 131` then `buffer type(3) not allowed` | harmless: the DMA-buf negotiation is refused on the output side, the next buffer type is used |
| `shailo: framesize mismatch A != B` | the input image size is not the HEF input size (e.g. `isp-out2` left at 640×480) |
| `shailo: N input stream(s) found, only one supported` | the HEF has several input streams (e.g. NV12 input): compile it with a single input |
| `nmsclient: frame N is not a byclass NMS frame` | wrong `-m`, `-c`, `-b` or `-S`, or `nmsclient` started in the middle of a frame (restart both) |
| `rectshm: shared memory of rectangles (X bytes) not found: Invalid argument` | a segment of another size is left from an older configuration: remove it with `ipcrm -m <id>` (see `ipcs -m`) and restart fastvideo |
| no rectangle at all | check with `nmsclient -n -v -s 0`: if no box appears even without threshold, the scene has nothing the model knows (80 COCO classes) or the input image is wrong |
| fastvideo does not stop | it is blocked in its FIFO write: stop it before `nmsclient`, or use `SIGKILL` |
| `hailort.log` in the current directory | written by HailoRT; on a full root filesystem, start fastvideo from a writable directory |
| fastvideo started with `-D` hangs when it stops (`futex` wait in `exit()`) | older fastvideo: the `-l` libraries were loaded before the daemon fork, and the thread that libhailort starts when it is loaded did not survive the fork; the libraries are now loaded after the fork |
| `nmsclient` runs but no rectangle is drawn, `sfastvideo: share memory allocation error` in the fastvideo log | older fastvideo: the shared memory with the id 0 (the first one created after the boot) was refused, and the GPU program used a private memory; fixed |
