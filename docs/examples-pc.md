# Examples on a PC

[← Back to README](../README.md)

These examples run on a desktop PC with a USB webcam, using the
[`uvc_desktop.json`](../data/uvc_desktop.json) configuration file.

## Webcam preview

Display the webcam image in a window rendered by the GPU:

```shell
$ fastvideo -D -j /etc/fastvideo/uvc_desktop.json -i uvc -o gpu
```

## ShaderToy filters

[ShaderToy](https://www.shadertoy.com) is a WebGL application that runs
fragment shaders to create visual effects. Fastvideo can apply filters
written for ShaderToy to live camera video, in real time and with good
performance.

To try the bundled filter:

```shell
$ fastvideo -j /etc/fastvideo/uvc_desktop.json -i uvc -o shadertoy
```

To use your own filter, replace the `MyShader.glsl` link:

```shell
$ sudo rm /usr/share/fastvideo/ShaderToy/MyShader.glsl
$ sudo cp myshader.glsl /usr/share/fastvideo/ShaderToy/
$ sudo ln -s myshader.glsl /usr/share/fastvideo/ShaderToy/MyShader.glsl
$ fastvideo -j /etc/fastvideo/uvc_desktop.json -i uvc -o shadertoy
```

## Software trigger

The [passthrough](modules.md#passthrough) module can act as a software
trigger: frames are only passed on when a shot is requested through
[`fastsetting`](applications.md#fastsetting).

```shell
$ fastvideo -D -j /etc/fastvideo/uvc_desktop.json -i uvc -t trigger -o gpu
$ fastsetting -j /etc/fastvideo/uvc_desktop.json -J /etc/fastvideo/shoot.json
```

## Performance check

The `dryrun` variant of the [passthrough](modules.md#passthrough) module
discards every frame. Combined with `-v`, it measures the raw frame rate of a
source:

```shell
$ fastvideo -v -j /etc/fastvideo/uvc_desktop.json -i uvc -o dryrun
```
