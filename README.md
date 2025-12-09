Fastvideo project
-----------------
Video library and applications' set.

The project offers 3 applications and 1 library for creating and managing video streams, which can be easily connected to a web server
The main feature is the use of dma_fd to transfer video from one device to another without copying to user space, if possible.

 - [fastvideo](#fastvideo)     : the main application create a stream from a json file;
 - [fastconfig](#fastconfig)   : it generates a json file with all hardware available on the host;
 - [fastsetting](#fastsetting) : open an unix socket to receive json objects that control the devices;

# Features

| Modules            |         | source | sync | transfer | control | dmafd | hw memory | soft memory | comment                      |
|:-------------------|:--------|:------:|:----:|:--------:|:-------:|:-----:|:---------:|:-----------:|:-----------------------------|
| [v4l2](#v4l2)      | capture | X      |      |          | X       | X     | X         |             | camera, isp ...              |
|                    | m2m     |        |      |  X       | X       | X     | X         |             | isp, en/decoder ...          |
|                    | output  |        | X    |          | X       | X     | X         |             | isp ...                      |
| [subv4l](#subv4l)  |         |        |      |          | X       |       |           |             | only control v4l2 media      |
| [v4l2_meta](#v4l2_meta)|     | X      |      |          |         | X     | X         |             | metadata stream from a media |
| [screen](#drm)     |         |        | X    |  X       | X       | X     | X         |             | hdmi, writeback              |
| [gpu](#egl)        | drm     |        | X    |  X       |         | X     | X         | X           | output pixels or Mesa        |
|                    | x11     |        | X    |  X       |         | X     | X         | X           |                              |
|                    | wayland |        | X    |  X       |         | X     | X         | X           |                              |
| [mpegts](#mpegts)  | udp     |        | X    |          |         |       |           | X           | may be read with ffplay      |
|                    | unix    |        | X    |          |         |       |           | X           | should use local client      |
|                    | file    |        | X    |          |         |       |           | X           | generate hls files           |
| [file](#file)      |         | X      | X    |          |         |       |           | X           | copy data from/to file       |
| [passthrough](#passthrough)| |        |      |  X       |         | X     | X         | X           | simple buffer transfer       |
|                    | dryrun  |        | X    |          |         | X     | X         | X           | lost the buffers for testing |
|                    | tee     |        |      |  X       |         | X     | X         | X           | send buffers into two syncs  |
|                    | convert |        |      |  X       |         |       |           | X           | apply cpu filter on images   |

## v4l2

Currently, this module does not support new multimedia devices. And the multimedia device linked to the v4l2 device must be configured with mediactl.
Few scripts are available for Raspberry Pi boards.

### The *json* structure

```json
  {
    "name": ["isp1-in","bcm2835-isp0-output0"],
    "type": "v4l2",
    "device": "/dev/video20",
    "definition": [
    ],
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

| entries      | types            | parent       | comment                                  |
|:--------     |:----------------:|:-------------|:-----------------------------------------|
| type         | string           | root         | must be "v4l2"                           |
| name         | string &#124; array | root      | give one or several name to the module   |
| device       | string           | root         | char device's path                       |
| definition   | object           | root         | input (source) definition                |
| transfer     | object           | root         | output (sync) definition for transfer    |
| fourcc       | string  | definition & transfer | pixels format                            |
| modifier     | integer | definition & transfer | pixels format modifier                   |
| width        | integer | definition & transfer | image's width                            |
| height       | integer | definition & transfer | image's height                           |
| stride       | integer | definition & transfer | image's pitch                            |
| fps          | integer         | root          | frame per second of the stream           |
| controls     | array of objects| root          | image settings                           |
| name         | string          | control       | setting's name                           |
| id           | integer         | control       | setting's v4l2ID                         |
| value        | integer         | control       | setting's value                          |

## subv4l

The new camera sensor's drivers often split the data stream and the sensor, focus lens, lights settings.

This modules are used by [fastsetting](#fastsetting) to control the device and not to generate a stream.

The modules description may be into the root object of json's file or into the "subdevices" array of a v4l2 object.

### The *json* structure

```json
 {
   "name": ["cam-ov5647","ov5647 10-0036"],
   "disable": true,
   "type": "subv4l",
   "device": "/dev/v4l-subdev0",
   "definition": {
     "fourcc": "GB10",
     "width": 1920,
     "height": 1080,
     "fps": 30
 },
```
The entries are the same as v4l2 modules.

## v4l2_meta

This modules open a special v4l2 device with metadata.

As the metadata and the data stream may come from the same device the module's name is different.

### The *json* structure

```json
  {
    "name": "video-control",
    "type": "v4l2_meta",
    "device": "/dev/video0"
  },
```
The entries are the same as v4l2 modules.

## egl

This module uses opengles and EGL to display images on the screen. The native driver can be “drm,” “X11,” “wayland,” or “offscreen.”

The module allows you to define a GLSL program on images and obtain images from the GPU.

 - pixels export: this method uses the “glReadPixels” function; it is slow but always available.
 - imagemesa export: this method uses the “eglExportDMABUFImageMESA” function; it is very fast but the output can be modified by the GPU tile system.

### The *json* structure

```json
  {
    "name":[ "gpu" ],
    "type":"gpu",
    "native":"drm",
    "device":"/dev/dri/card0",
  },
```

| entries      | types            | parent       | comment                                  |
|:--------     |:----------------:|:-------------|:-----------------------------------------|
| type         | string           | root         | must be "gpu"                            |
| name         | string &#124; array | root      | give one or several name to the module   |
| native       | string           | root         | native api : "drm", "x11", "wayland"     |
| device       | string           | root         | char device's path for "drm"             |
| definition   | object           | root         | input (source) definition                |
| transfer     | object           | root         | output (sync) definition for transfer    |
| fourcc       | string  | definition & transfer| pixels format                             |
| modifier     | integer | definition & transfer| pixels format modifier                    |
| width        | integer | definition & transfer| image's width                             |
| height       | integer | definition & transfer| image's height                            |
| stride       | integer | definition & transfer| image's pitch                             |
| programs     |array of objects | root         | GLSL program definition                   |
| vertex       | string          | programs     | GLSL vertex file's path                   |
| fragement    | string          | programs     | GLSL fragment file's path                 |
| export       | string          | root         | export api : "pixels", "imagemesa"        |

## drm

This module uses the *drm* device to display the image on the screen. It allows the writeback connector to be used to transfer images and can control image rotation.

### The *json* structure

```json
  {
    "name":[ "screen" ],
    "type":"screen",
    "device":"/dev/dri/card0",
    "controls": [
      {
		  "name": "rotation",
		  "value": "reflect"
	  }
    ]
  },
```

| entries      | types            | parent       | comment                                  |
|:--------     |:----------------:|:-------------|:-----------------------------------------|
| type         | string           | root         | must be "drm"                            |
| name        | string &#124; array | root       | give one or several name to the module   |
| device       | string           | root         | char device's path for "drm"             |
| definition   | object           | root         | input (source) definition                |
| transfer     | object           | root         | output (sync) definition for transfer    |
| fourcc       | string      | definition & transfer | pixels format                        |
| modifier     | integer     | definition & transfer | pixels format modifier               |
| width        | integer     | definition & transfer | image's width                        |
| height       | integer     | definition & transfer | image's height                       |
| stride       | integer     | definition & transfer | image's pitch                        |
| controls    | array of objects | root          | image settings                           |
| name         | string          | controls      | setting's name must be "rotation"        |
| value     | string &#124; integer | controls   | may be "reflect", "90", "180" or "270"   |

## mpegts

This module generates a MPEG2-ts stream. The sync input must be a h264 stream.

### The *json* structure

```json
  {
    "name":[ "dvb-i" ],
    "type":"mpegts",
    "proto":"udp",
    "host": "192.168.176.14",
    "port": 5024,
    "periodic":60
  },
```

| entries      | types            | parent       | comment                                  |
|:--------     |:----------------:|:-------------|:-----------------------------------------|
| type         | string           | root         | must be "mpegts"                            |
| name        | string &#124; array | root       | give one or several name to the module   |
| proto        | string           | root         | may be "udp", "unix", "file"             |
| host         | string           | root         | the destination address or socket's path |
| port         | integer          | root         | the port number for "udp" protocol       |
| periodic     | integer          | root         | the number of frames between each I-frame|

## passthrough

This module is by default a simple serving plate between 2 others modules. But some options change that.

 - "dryrun" trashs the stream and the module becomes a "sync" output.
 - "tee" allows to send the stream to 2 output's modules.
 - "copy" allows to dump the stream before serving to the output.
 - "convert" allows to set a filter function from another library.

### The *json* structure

```json
  {
    "name": "untile",
    "type": "passthrough",
    "convert": "untile",
    "library": "/usr/lib/fastvideo/libconvertuntile.so"
  },
```

| entries      | types            | parent       | comment                                  |
|:--------     |:----------------:|:-------------|:-----------------------------------------|
| type         | string           | root         | must be "passthrough"                    |
| name        | string &#124; array | root       | give one or several name to the module   |
| branch       | object           | root         | output's module description              |
| name         | string           | branch       | the module's name                        |
| type         | string           | branch       | the module's type "v4l2", "gpu", "screen"|
| convert      | string &#124; object | root     | see [convert library](#convert-library)  |
| controls     | array of objects | root         | image settings                           |
| dryrun       | boolean          | controls     | trash the stream                         |
| tee          | boolean          | controls     | enable the branch description            |

## file

This module allows to push data into or fromto, file or fifo.


| entries      | types            | parent       | comment                                  |
|:--------     |:----------------:|:-------------|:-----------------------------------------|
| type         | string           | root         | must be "passthrough"                    |
| name        | string &#124; array | root       | give one or several name to the module   |
| path         | string           | root         | directory where the file is used         |
| filename     | string           | root         | filename                                 |
| mode        | string &#124; array | root       | "regular" or "fifo"                      |

### The *json* structure

```json
  {
    "name": "file",
    "type": "file",
  };
```

# Applications

## Fastvideo

This application reads a json file and creates streams following its arguments.

```bash
 $ fastvideo -j /etc/fastvideo/raspicam.json -i cam-ov5647 -o isp-in -i isp-out -o gpu
```

### The arguments

| arguments | type | example                    | comment                                |
|:----------|:----:|:---------------------------|:---------------------------------------|
| -W        | path | -W /etc/fastvideo          | give the root path of execution        |
| -D        |      | -D                         | run application as daemon              |
| -j        | path | -j /etc/fastvideo/uvc.json | set the application configuration file |
| -i        | name | -i cam-ov5647              | set a module's name for a source       |
| -o        | name | -o isp-in     | set a module's name for the sync of previous source |
| -t        | name | -t enc-h264 |set a module's name for the transfer of previous source|
| -w        | int  | -w 640                     | set the images' width of the source    |
| -h        | int  | -h 480                     | set the images' height of the source   |
| -L        | path | -L /var/log/fastvideo.log  | set the log file's path                |

## Fastconfig

This application generates a json file from devices information. It uses the *media* devices for v4l2 modules.

```bash
 $ fastconfig
```

It's important to open the generated file to customize it.

Some customized's files are available into *data* directory.

### The arguments

| arguments | type | example                    | comment                                |
|:----------|:----:|:---------------------------|:---------------------------------------|
| -o        | path | -o myconfig.json           | the output file's path                 |
| -a        |      | -a                         | set all informations about devices     |
| -m        | name | -m media0                  | set information for only one media     |
| -v        | path | -v /dev/video0             | set information for only one v4l2      |
| -d        | path | -d /dev/dri/Card0          | set information for only one drm       |

## Fastsetting

This application reads the json file and creates a UNIX socket to wait json object containing "controls" information

```bash
 $ fastsetting -j /etc/fastvideo/raspicam.json
```

When a client connects the server, it sends the streams capabilities (all modules availables and their controls).

### The *json* structure

```json
{"name":"cam-ov5647","controls":[{"id":9963790,"value": 500}]}
```

### The arguments

| arguments | type | example                    | comment                                |
|:----------|:----:|:---------------------------|:---------------------------------------|
| -W        | path | -W /etc/fastvideo          | give the root path of execution        |
| -D        |      | -D                         | run application as daemon              |
| -j        | path | -j /etc/fastvideo/uvc.json | set the application configuration file |
| -L        | path | -L /var/log/fastsetting.log| set the log file's path                |

# Convert library

This is a C library that must create and fill a *Convert_t* structure.

```C
Convert_t convert_rgba =
{
  .name = "bayer2rgb",
  .copy = 1,
  .fourcc_in = 0,
  .fourcc_out = FOURCC('A','B','2','4'),
  .ops =
  {
    .create = convert_create,
    .convert = convert_convert,
    .destroy = convert_destroy,
  },
};
```

And a initialization function must register this structure.

```C
#include <dlfcn.h>

static void __attribute__ ((constructor)) convert_init()
{
	spassthrough_convert_append_t _spassthrough_convert_append = NULL;
	void *hdl = dlopen("libfastvideo.so", RTLD_NOW);
	if (hdl != NULL)
		_spassthrough_convert_append = dlsym(hdl, "spassthrough_convert_append");
	if (_spassthrough_convert_append)
	{
		_spassthrough_convert_append(&convert_rgba);
	}
}
```

# Building

The project uses only GNU Makefile, gcc (or clang). The *defconfig* file contains the available compilation options. For more information, see the [Makemore project](https://github.com/mchalain/makemore).

```bash
 $ make BUILDDIR=$PWD/build DRM=n prefix=/usr sysconfdir=/etc/fastvideo defconfig
 $ cd build
 $ make
 $ make DESTDIR=$PWD/tempo install
```

Other interesting configuration's options :

 - CROSS\_COMPILE=arm-none-linux-gnueabi
 - SYSROOT=/opt/arm-none-linux-gnueabi-sdk/arm-none-linux-gnueabi/sysroot

## Contribute

You can find a module's skeleton into the sources directory, to start a new module.

A lot modules are missing but the main goal is the speed on light boards, and the currently all video devices are supported.
It should be easy to push the stream from the last device into another application that uses the CPU.

## Missing

## Automatic White Balance, Automatic Exposure Algo

The project offers the *sv4l2\_meta* to unpack camera metadata and send command to the *fastsetting* application to change the controls.
But the algorithms are missing.

## Video extraction from GPU

The *gpu* module allows to stream out the EGL Texture as a bitmap in CPU memory, or as a dma buffer. The second case, is more efficient but the embedded GPU use a tiled image format.

## Video convertion intp CPU

The current *convert* plugins are not ready.

## Configuration

As the V4L2 system may be a succession of link between devices and subdevices, the naming of each *v4l2* or *subdev* object is complex and the code is not really clear.
This part of configuration may be refactored.

# testing
## Raspberry Pi 3/4

The project offers configuration files to use with Raspberry Pi and Broadcom ISP, the [main file](data/raspicam.json) contains default configuration for camera, isp, gpu, screen. Several camera modules are supported and needs settings file.

By default the output is the GPU rendering into X11 window. The native rendering available are *X11*, *wayland*, *drm* and *offscreen*, the choice is done by the first entry into the *native* table of the *gpu* object.

### Raspberry Pi Camera Module 1

This camera uses a ov5647 camera sensor. The following command lines should start a stream

```bash
$ fastvideo -j /etc/fastvideo/raspicam.json -i cam-ov5647 -o isp-in -i isp-out -o gpu &
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-ov5647.json
```

### Raspberry Pi Camera Module 3

The camera uses a imx708 camera sensor. It needs to stream the image and the metadata at the same time. The following command lines should start a stream

```bash
$ fastvideo -j /etc/fastvideo/raspicam.json -i cam-imx708 -o isp-in -i unicam-embedded -o dryrun -i isp-out -o gpu &
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-imx708.json
```

### Raspberry Pi Camera Module HQ

The camera uses a imx477 camera sensor. It needs to stream the image and the metadata at the same time. The following command lines should start a stream

```bash
$ fastvideo -j /etc/fastvideo/raspicam.json -i cam-imx477 -o isp-in -i unicam-embedded -o dryrun -i isp-out -o gpu &
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-imx477.json
```

### Raspberry Pi Camera Module GS

This camera uses a imx296 camera sensor. The following command lines should start a stream

```bash
$ fastvideo -j /etc/fastvideo/raspicam.json -i cam-imx296 -o isp-in -i isp-out -o gpu &
$ fastsetting -j /etc/fastvideo/raspicam.json -J /etc/fastvideo/setting-imx296.json
```
