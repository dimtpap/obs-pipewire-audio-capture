# Audio device and application capture for OBS Studio using PipeWire 

This plugin adds 3 sources for capturing audio outputs, inputs and applications using [PipeWire](https://pipewire.org)
## Binary installation
Get the `linux-pipewire-audio-(version).tar.gz` archive from the [latest release](https://github.com/dimtpap/obs-pipewire-audio-capture/releases/latest)  
If OBS Studio is installed as a
- Regular package: Extract the archive in `~/.config/obs-studio/plugins/`
- Flatpak: Extract the archive in `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/`

## Building and installing
Ensure you have PipeWire and OBS Studio development packages, then run these commands
```sh
cmake -B build -DCMAKE_INSTALL_PREFIX="/usr" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cd build
make
make install #May need root
```
