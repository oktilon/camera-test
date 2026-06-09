# Camera test utility

## Requirements

### Build system

[Meson build](https://mesonbuild.com/) v.0.56+

### Cross build requirements

- Emak sysroot
- Linaro toolchain `gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu`

## Build

### Native build

```bash
meson setup build
ninja -C build
```

### Cross build

```bash
meson setup build --cross-file=emak.ini
ninja -C build
```

## Usage

Default device is `/dev/video0`, alternatively could be specified as first argument:

```bash
./camera-test /dev/videoX
```
