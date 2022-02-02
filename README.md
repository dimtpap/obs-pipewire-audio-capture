# PipeWire Audio Capturing for OBS Studio
### Do not use this for production
**Experimental** PipeWire audio capturing for OBS Studio  
This plugin adds 3 sources for capturing outputs, inputs and applications using PipeWire  
There is no real need for this currently, `pipewire-pulse` works perfectly with OBS's PulseAudio implementations

## Issues
- This currently conflicts with the PipeWire screen/window capturing. Expect crashes if you're using sources from both
- Generally there needs to be more error handling and polishing

Pre-built binaries are available in the [releases](https://github.com/Qufyy/obs-pipewire-audio-capture/releases)
## Building
Build this as part of OBS Studio, its instructions are [here](https://obsproject.com/wiki/install-instructions#linux)
- Checkout this repo to a directory named `linux-pipewire-audio` inside the OBS `plugins` folder  
- Add `add_subdirectory(linux-pipewire-audio)` under `elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")` in OBS's `plugins/CMakeLists.txt` file  
- Build OBS Studio
