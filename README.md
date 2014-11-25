# gst-rtsp-server-wfd

This module is implemented to run miracast(wifi-display) source side using gstreamer. (especially, gstreamer 1.2.3)
This is basically inherited from gst-rtsp-server ( http://cgit.freedesktop.org/gstreamer/gst-rtsp-server/ )

### Pre-conditions:
This module is running on established p2p connection with wifi direct, which means that you have to setup this network environment to run this module.
I hope this link would be very helpful. ( http://cgit.freedesktop.org/~dvdhrm/miracle )

### Build requires:
gstreamer
gst-plugins-base

### Test
examples/wfd-test ( currently, rtsp port number is 2022 )

### WFD pipeline:

ximagesrc ! videoscale ! videoconvert ! "video/x-raw,width=640,height=480,framerate=30/1" ! x264enc aud=false byte-stream=true bitrate=512 ! "video/x-h264,profile=baseline" ! mpegtsmux name=mux pulsesrc device=alsa_output.pci-0000_00_1b.0.analog-stereo.monitor ! audioconvert !  faac ! mux. mux. ! rtpmp2tpay name=pay0 pt=33

##### Used plugins in the wfd pipeline:
- gst-plugins-base : videoscale, videoconvert, audioconvert, rtpmp2tpay
- gst-plugins-ugly : x264enc
- gst-plugins-good : ximagesrc, pulsesrc, rtpbin, multiudpsink
- gst-plugins-bad : mpegtsmux, faac

### Build steps:
1. Build and install all gstreamer core and plugins described above.
2. ./autogen.sh --prefix=/usr/local
3. make
4. sudo make install

##### Tested 
1. tested on ubuntu 12.04
2. tested sinks : samsung cavium dongle, samsung homesync.

### TODO:
- Define WFD rtsp message - ongoing
- Add logic to make decision spec with WFD rtsp message in M3 stage.
- Fix FIXME code. (check FIXME-WFD, TODO-WFD)
- Tuning each plugin to improve performance.
