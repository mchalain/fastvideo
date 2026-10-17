# Building and installing

[← Back to README](../README.md)

## Dependencies

Fastvideo depends on:

- [Jansson](https://github.com/akheron/jansson) (JSON parsing);
- OpenGL ES 2 and EGL;
- libdrm and GBM;
- X11 or Wayland (optional, for windowed rendering).

On a Debian-like system:

```shell
$ sudo apt install libjansson-dev wayland-protocols libdrm-dev libgbm-dev libgles2-mesa-dev libegl1-mesa-dev
```

## Compilation

The project uses only GNU Make and GCC (or Clang), through the
[makemore](https://github.com/mchalain/makemore) build system. The
`defconfig` file lists the available build options (`EGL`, `DRM`, `DVB`,
`IMAGEMESA`, …); each one is enabled with `y` or disabled with `n`.

### In-tree build

The simplest build is:

```shell
$ make defconfig
$ make
$ make install
```

### Out-of-tree build

To keep the sources clean and set the installation paths:

```shell
$ make BUILDDIR=$PWD/build prefix=/usr sysconfdir=/etc/fastvideo defconfig
$ cd build
$ make
$ make DESTDIR=$PWD/tempo install
```

### Debug build

`DEBUG=y` adds debug symbols and extra traces:

```shell
$ make defconfig
$ make DEBUG=y
```

### Cross compilation

Set `CROSS_COMPILE` to the toolchain prefix and `SYSROOT` to the target
system root:

```shell
$ make BUILDDIR=$PWD/build-arm \
       CROSS_COMPILE=arm-none-linux-gnueabi \
       SYSROOT=/opt/arm-none-linux-gnueabi-sdk/arm-none-linux-gnueabi/sysroot \
       defconfig
$ cd build-arm
$ make
$ make DESTDIR=$PWD/tempo install
```

## Installation paths

The installation directories are chosen at the configuration stage
(`make ... defconfig`), using the standard variables:

| Variable      | Contents                                          | Default                                   |
|:--------------|:--------------------------------------------------|:------------------------------------------|
| `prefix`      | Main installation directory                       | `/usr/local`                              |
| `exec_prefix` | Main directory for binaries                       | `$prefix`                                 |
| `bindir`      | Applications (`fastvideo`, `fastsetting`, …)      | `$exec_prefix/bin`                        |
| `libdir`      | Library (`libfastvideo.so`)                       | `$exec_prefix/$ARCH/lib` (`ARCH` depends on the OS) |
| `sysconfdir`  | Configuration files (`*.json`)                    | `$prefix/etc`                             |
| `datadir`     | Other data files (`*.glsl`, …)                    | `$prefix/share/fastvideo`                 |
