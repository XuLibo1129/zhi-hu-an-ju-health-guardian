# wifi_csi_rx

ESP32-S3 Wi-Fi CSI receiver. It starts a SoftAP, receives UDP packets from the
TX board, enables Wi-Fi CSI, and outputs six-channel CSI features:

```text
rssi,mean_amp2,amp2_8,amp2_24,amp2_48,amp2_80
```

The features can be printed to serial and forwarded to the PC inference tool
over UDP.

When AP+STA is enabled, the A board may also receive CSI from the phone hotspot
router link. The firmware therefore enables `CSI source MAC filter = auto` by
default. In auto mode it prefers MAC addresses of stations connected to the A
board SoftAP, so the B-board link is forwarded and the phone-hotspot link is
dropped. If no SoftAP station is known yet, it falls back to short auto-learning.

Default link:

```text
SSID: CSI_RX_AP
Password: configure it in `idf.py menuconfig` before flashing.
Channel: 6
SoftAP IP: 192.168.4.1
UDP port: 3333
Feature UDP target: configure your computer IP, port 34567
Max clients: 2
```

Keep the PC on the same router/hotspot as the A-board STA and let the PC GUI
listen on UDP port `34567`.

If auto-learning picks the wrong source, set `CSI source MAC filter` in
`idf.py menuconfig` to the B-board MAC, for example:

```text
e0:72:a1:d2:42:68
```

For final deployment, enable AP+STA in `idf.py menuconfig`:

- `Also connect A board to router as STA`: enabled
- `Router STA SSID` / `Router STA password`: router or phone hotspot
- `CSI feature UDP target IP`: PC IP on the router LAN

B board still connects to `CSI_RX_AP`; the PC and main ESP32-P4 terminal stay
on the router network.

Build:

```powershell
idf.py set-target esp32s3
idf.py build
```

Expected serial output:

```text
-42,1234,99,1200,880,240
I (...) wifi_csi_rx: STAT csi_hz=50 udp_hz=50 forward_hz=50 ...
```
