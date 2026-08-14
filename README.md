# LVC Firmware

This repo hosts the firmware for SEB's Live Video Controller.
It uses an OV5647 camera and an ESP32-P4 to send H.264 encoded video
down to the ground via MPEG-TS.

## Recieving

In order to stream the video locally,
run the following command:

`ffplay "udp://@232.10.11.12:3333?localaddr=<LOCAL_ADDR>&pkt_size=188" -f mpegts -analyzeduration 500000`, where `<LOCAL_ADDR>` is the local address of the interface
used to connect to LVC. 

In order to save the video to a file,
run the following commands:

 - `netcat -ul -p 3333 -s 232.10.11.12 > output.ts`
 - `ffmpeg -f mpegts -i output.ts -vcodec copy output.mp4`

## Configuration

You can configure the project with ESP-IDF menuconfig.

> [!CAUTION]
> Enable the `Select ESP32-P4 revisions <3.0 (No >=3.x Support)` option in `Component config > Hardware Settings > Chip revision`.
> Failure to do so may result in a bricked LVC board.

In addition to the chip revision,
there are a few more options that must be selected for the program to function:

 - Enable `Component config > ESP PSRAM > Support for external PSRAM`
 - Enable `Component config > Espressif Camera Sensors Configurations > Camera Sensor Configuration > Select and Set Camera Sensor > OV5647`
 - Set the desired format in the aforementioned `OV5647` submenu.

Finally, there are a number of options that you may adjust to find the most optimal broadcast in the `LVC Configuration` menu:

`LVC Configuration > Video Configuration`
| Option | Description | Default |
|--------|-------------|---------|
| `Camera Width (px)` | The width of the camera stream in pixels. Set to the `OV5647` format. | `1280` |
| `Camera Height (px)` | The height of the camera stream in pixels. Set to the `OV5647` format. | `960` |
| `Encoder FPS` | The FPS output from the H.264 encoder. Can be less than the `OV5647` format. | `15` |
| `H264 GOP` | The number of frames in each group of pictures. Smaller numbers mean keyframes appear more often | `15` |
| `H264 Minimum QP` | The minimum quantization parameter for the encoder. Smaller numbers mean bigger intermediate packet sizes. | `26` |
| `H264 Maximum QP` | The maximum quantization parameter for the encoder. Smaller numbers mean bigger keyframe sizes | `35` |
| `H264 Bitrate` | The bitrate of the encoder. This does not account for the network or MPEG-TS overhead.


`LVC Configuration > Network Configuration`
| Option | Description | Default |
|--------|-------------|---------|
| `Multicast IPv4 Address (tx)` | The multicast IPv4 address the packets are transmitted to | `232.10.11.12` |
| `Multicast Port (tx)` | The multicast port the packets are transmitted to | `3333` |
| `Multicast TTL` | The multicast time to live. Specifies the number of router hops the packets can take | `1` |
| `Static IP` | Set if a static IP is required, such as on a local machine. Disable for P3 | [x] | 
| `Device IP Address (IPv4)` | The static IPv4 address assigned to LVC | `169.254.41.21` |
| `Device Netmask (IPv4) | The netmask for the static IPv4 address | `255.255.255.0` |
| `Device Gateway Address (IPv4) | The default gateway for the static IPv4 address | `169.254.41.1` |
