# Audio device and application capture for OBS Studio using PipeWire 

This plugin adds 3 sources for capturing audio outputs, inputs and applications using [PipeWire](https://pipewire.org)
![Device capture properties](assets/device-capture.png)
![App capture properties](assets/app-capture.png)

## Usage
### Requirements
- OBS Studio 28.0 or later
- [WirePlumber](https://pipewire.pages.freedesktop.org/wireplumber/)

PipeWire 0.3.62 or later is highly recommended ([#17](https://github.com/dimtpap/obs-pipewire-audio-capture/issues/17), [PipeWire#2874](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/2874))

For the plugin to be able to capture applications, PipeWire should be set up to handle audio on your system.
For most applications, the [`pipewire-pulse`](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/FAQ#should-i-uninstall-everything-pulseaudio)
compatibility layer should be enough, but there are also `pipewire-jack` and `pipewire-alsa`.
If applications aren't showing up in the plugin, your system may be missing one of those components.  
See the [PipeWire wiki](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/home) for more info.

### Binary installation
Get the `linux-pipewire-audio-(version).tar.gz` archive from the [latest release](https://github.com/dimtpap/obs-pipewire-audio-capture/releases/latest)  
If OBS Studio is installed as a
- Regular package: Extract the archive in `~/.config/obs-studio/plugins/`
- Flatpak: Extract the archive in `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/`  
  - Note: If the plugin isn't working try running OBS using `flatpak run --filesystem=xdg-run/pipewire-0 com.obsproject.Studio`
  or run `flatpak override --filesystem=xdg-run/pipewire-0 com.obsproject.Studio` and then open OBS as usual

### Building and installing
Ensure you have CMake, PipeWire and OBS Studio/libobs development packages, then in the repo's root:
```sh
cmake -B build -DCMAKE_INSTALL_PREFIX="/usr" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cd build
make
make install #May need root
```
## Inclusion in upstream OBS Studio

This plugin is currently in the process of being worked on to merge into upstream OBS Studio. See https://github.com/obsproject/obs-studio/pull/6207
