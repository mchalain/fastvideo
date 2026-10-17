# Contributing and roadmap

[← Back to README](../README.md)

## Writing a new module

A module skeleton, [`sskeleton.c`](../src/sskeleton.c), is available in the
sources directory as a starting point for a new module.

Many modules are still missing, but the project's main goal is speed on
low-power boards, and all the usual video devices are already supported. It
should be straightforward to push the stream from the last device of a
pipeline into another application that processes it on the CPU.

## Roadmap

### Automatic white balance and automatic exposure

The [v4l2](modules.md#v4l2) module unpacks camera metadata (`isp-meta`) and sends commands to
[`fastsetting`](applications.md#fastsetting) to update the controls, but the
algorithms themselves are still missing (only a basic AE is available in
[`rpivc4_alg`](raspberry-pi.md#3a-algorithms)).

### Video extraction from the GPU

The [gpu](modules.md#gpu) module can export the EGL texture either as a
bitmap in CPU memory or as a DMA buffer. The latter is more efficient, but
embedded GPUs use a tiled image format that the next stage must handle.

### CPU video conversion

The current *convert* plugins are not production-ready yet.

### Configuration

Because the V4L2 system may be a long chain of links between devices and
sub-devices, naming each *v4l2* or *subv4l* object is complex and the
corresponding code is not very clear. This part of the configuration may be
refactored.
