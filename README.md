# PipeWire Audio Capturing for OBS Studio
## Use this for production at your own risk
**Experimental** PipeWire audio capturing for OBS Studio  
This plugin adds 3 sources for capturing audio outputs, inputs and applications using PipeWire
## Issues
- It currently conflicts with the PipeWire screen/window captures. Expect crashes if you're using sources from both.
- It only captures one stream per application, this is being worked on.
- Generally there needs to be more error handling and polishing.
## Installation
If you're using pre-built binaries from the [releases](https://github.com/Qufyy/obs-pipewire-audio-capture/releases) extract the archive in `~/.config/obs-studio/plugins/`

## Building
To build this on its own ensure you have PipeWire and OBS development packages, then run these commands
```sh
cmake -B build -DBUILD_STANDALONE=On -DCMAKE_INSTALL_PREFIX="/usr" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cd build
make
make install #May need root
```
---
To build this as part of OBS Studio first follow [these instructions](https://obsproject.com/wiki/build-instructions-for-linux), then
- Clone this repo to a directory named `linux-pipewire-audio` inside the OBS `plugins` folder  
- Add `add_subdirectory(linux-pipewire-audio)` under `elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")` in OBS's `plugins/CMakeLists.txt` file  
- Build OBS Studio
