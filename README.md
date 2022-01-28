# PipeWire Audio Capturing for OBS Studio
### Do not use this for production
**Experimental** PipeWire audio capturing for OBS Studio  
This plugin adds 3 sources for capturing outputs, inputs and applications using PipeWire  
There is no real need for this currently, `pipewire-pulse` works perfectly with OBS's PulseAudio implementations

## Known issues
- This currently conflicts with the PipeWire screen/window capturing - If you have sources from both and you remove them in a session, 
it will crash (most likely due to both calling `pw_deinit()`)
- Selecting an application that is two or more times in the list (e.g. Firefox tabs) only connects to the first one
- Generally there needs to be more error handling and polishing

Pre-built binaries are available in the [releases](https://github.com/Qufyy/obs-pipewire-audio-capture/releases)
## Building
Build this as part of OBS Studio, its instructions are [here](https://obsproject.com/wiki/install-instructions#linux)
- Checkout this repo to a directory named `linux-pipewire-audio` inside the OBS `plugins` folder  
- Add `add_subdirectory(linux-pipewire-audio)` under `elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")` in OBS's `plugins/CMakeLists.txt` file  
- Build OBS Studio
