# PipeWire Audio Capturing for OBS Studio

PipeWire audio capturing for OBS Studio  
This plugin adds 3 sources for capturing audio outputs, inputs and applications using PipeWire
## Pre-built Installation
If you're using pre-built binaries from the [releases](https://github.com/Qufyy/obs-pipewire-audio-capture/releases) extract the archive in `~/.config/obs-studio/plugins/`

## Building and installing
Ensure you have PipeWire and OBS Studio development packages, then run these commands
```sh
cmake -B build -DCMAKE_INSTALL_PREFIX="/usr" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cd build
make
make install #May need root
```
---
