# wifi_csi_tx

ESP32-S3 Wi-Fi CSI transmitter. It connects to the receiver SoftAP and sends
UDP packets at a fixed interval so the receiver can collect CSI.

Default link:

```text
SSID: CSI_RX_AP
Password: configure it in `idf.py menuconfig` before flashing.
Receiver IP: 192.168.4.1
UDP port: 3333
Packet interval: 20 ms
```

Build:

```powershell
idf.py set-target esp32s3
idf.py build
```
