# Applications

[← Back to README](../README.md)

- [fastvideo](#fastvideo) – runs the streams
- [fastconfig](#fastconfig) – discovers the hardware
- [fastsetting](#fastsetting) – controls the devices at run time

## fastvideo

`fastvideo` reads a JSON configuration file and builds the streams described
by its command-line arguments.

```shell
$ fastvideo -j /etc/fastvideo/raspicam.json -i cam-ov5647 -o isp-in -i isp-out -o gpu
```

### Building a stream

Each module is referred to by one of the names given in the JSON file. A
stream always starts with a source (`-i`) and ends with a sink (`-o`);
transfers (`-t`) may be inserted in between:

```shell
$ fastvideo -j config.json -i <source> [-t <transfer> ...] -o <sink> [-i <source> ... -o <sink>]
```

Several streams can run in the same process: each new `-i` starts a new
stream.

Some modules accept an option directly after their name, separated by a
colon, for example:

```shell
$ fastvideo -j fastconfig.json -i cam -o file:./test.mjpeg
$ fastvideo -j raspi5cam.json -i raw-ov9281 -o gpu:fourcc="R16 "
```

### Options

| Option | Argument | Example                       | Description                                              |
|:-------|:--------:|:------------------------------|:---------------------------------------------------------|
| `-j`   | path     | `-j /etc/fastvideo/uvc.json`  | Configuration file.                                      |
| `-i`   | name     | `-i cam-ov5647`               | Module used as the source of a new stream.               |
| `-t`   | name     | `-t enc-h264`                 | Module used as a transfer stage of the current stream.   |
| `-o`   | name     | `-o isp-in`                   | Module used as the sink of the current stream.           |
| `-W`   | path     | `-W /etc/fastvideo`           | Working directory (default: the data directory).         |
| `-D`   |          | `-D`                          | Run as a daemon.                                         |
| `-L`   | path     | `-L /var/log/fastvideo.log`   | Log file.                                                |
| `-P`   | path     | `-P /run/fastvideo.pid`       | PID file (daemon mode).                                  |
| `-U`   | user     | `-U video`                    | User that owns the process.                              |
| `-l`   | path     | `-l libmyplugin.so`           | Load an additional plugin library.                       |
| `-v`   |          | `-v`                          | Verbose mode (displays the frame rate, among others).    |
| `-I`   |          | `-I`                          | Initialise the devices, then exit without streaming.     |

## fastconfig

`fastconfig` generates a JSON configuration file from the devices found on
the host. It uses the *media* devices to describe the V4L2 modules.

```shell
$ fastconfig -o myconfig.json
```

The generated file is a starting point: always open it and adapt it to your
needs. Ready-made configuration files are available in the
[`data/`](../data) directory.

### Options

| Option | Argument | Example              | Description                                    |
|:-------|:--------:|:---------------------|:-----------------------------------------------|
| `-o`   | path     | `-o myconfig.json`   | Output file.                                   |
| `-a`   |          | `-a`                 | Include all the information about the devices. |
| `-m`   | name     | `-m media0`          | Describe only one media device.                |
| `-v`   | path     | `-v /dev/video0`     | Describe only one V4L2 device.                 |
| `-d`   | path     | `-d /dev/dri/card0`  | Describe only one DRM device.                  |

## fastsetting

`fastsetting` reads the JSON configuration file and opens a UNIX socket,
waiting for JSON objects that carry *controls*.

```shell
$ fastsetting -j /etc/fastvideo/raspicam.json
```

When a client connects, the server first sends the stream capabilities: all
the available modules and their controls. The client can then send control
requests such as:

```json
{"name":"cam-ov5647","controls":[{"id":9963790,"value": 500}]}
```

Settings files (for example `setting-imx477.json`) can be applied at start-up
with `-J`.

### Options

| Option | Argument | Example                             | Description                                       |
|:-------|:--------:|:------------------------------------|:--------------------------------------------------|
| `-j`   | path     | `-j /etc/fastvideo/uvc.json`        | Configuration file.                               |
| `-J`   | path     | `-J /etc/fastvideo/setting-ov5647.json` | Settings file to apply (may be repeated).     |
| `-s`   | path     | `-s /tmp/camera/control`            | Path of the UNIX socket (default set by `FASTSETTING_DEFAULT_SERVER` in `defconfig`). |
| `-W`   | path     | `-W /etc/fastvideo`                 | Working directory.                                |
| `-D`   |          | `-D`                                | Run as a daemon.                                  |
| `-L`   | path     | `-L /var/log/fastsetting.log`       | Log file.                                         |
| `-P`   | path     | `-P /run/fastsetting.pid`           | PID file (daemon mode).                           |
| `-I`   |          | `-I`                                | Apply the settings, then exit without opening the socket. |
