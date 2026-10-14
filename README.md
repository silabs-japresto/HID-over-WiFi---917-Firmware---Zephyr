# HID over Wi-Fi firmware for SiWG917 (Zephyr)

> **Example only:** This code is an example and is not intended for production devices. Review and address security considerations before adapting it for production use.

Zephyr application for the SiWG917 BRD4338A radio board. The two kit buttons send gamepad A and B states to the [HID over Wi-Fi Host](https://github.com/silabs-japresto/HID-over-WiFi---Host) receiver. BTN0 maps to HID Button usage 1 (A); BTN1 maps to Button usage 2 (B). The board's joystick is unavailable with this radio board.

The board advertises as `HID-WIFI-917` over BLE for Wi-Fi provisioning. It remembers Wi-Fi credentials and BLE bonds in Zephyr settings storage, rejoins Wi-Fi after reset, and sends six-byte UDP packets to a configured LAN receiver. Buttons are polled every 5 ms. A packet is sent on state change and every 50 ms while connected. The host releases buttons after 250 ms without packets.

## Build

Install [Simplicity SDK for Zephyr v2026.6.0](https://docs.silabs.com/zephyr/latest/zephyr-getting-started/getting-started-guide) as a separate West workspace:

```sh
west init -m https://github.com/SiliconLabsSoftware/zephyr-silabs --mr v2026.6.0 zephyr-workspace
cd zephyr-workspace
west update
west packages pip --install
west blobs fetch hal_silabs
west sdk install -t arm-zephyr-eabi
```

Clone this application separately. Copy `local.conf.example` to `local.conf`, then set `CONFIG_HID_WIFI_HOST_IPV4` to the receiver's LAN IPv4 address. `local.conf` is ignored by Git. From the West workspace, build with:

```sh
west build -p always -b siwx917_rb4338a /path/to/this/application -- -DEXTRA_CONF_FILE=local.conf
```

The build output is `build/zephyr/zephyr.rps`. Flash with `west flash` using the debug adapter. The SiWG917 also needs compatible NWP connectivity firmware for the SDK release; see the Silicon Labs [board and firmware setup guide](https://docs.silabs.com/zephyr/latest/zephyr-getting-started/getting-started-guide).

## Provision and receive

Install the Python host application from the [host repository](https://github.com/silabs-japresto/HID-over-WiFi---Host). Run its `provision.py --ssid YOUR_WIFI_NAME` command and enter the Wi-Fi password when prompted. Then start its GUI or headless receiver on the configured computer. The board and receiver must be on the same reachable LAN. The BLE write characteristic requires an encrypted connection. This inputless board uses Just Works pairing, so the link is encrypted but pairing does not authenticate the peer.

The BLE service UUID is `0xAABB`. The encrypted write characteristic `0x1AA1` accepts up to 20 bytes per write: `0x01, offset, SSID bytes`, `0x02, offset, password bytes`, and finally `0x03` to commit. The readable status characteristic `0x1BB1` reports 0 waiting, 1 joining, 2 connected, 3 join failed, or 4 invalid command. The host tool handles chunking and pairing.

Each UDP datagram on port 5004 is six bytes: ASCII `HW`, protocol version 1, wrapping sequence number, button bitmask (`bit 0 = A`, `bit 1 = B`), and reserved zero. The receiver treats each datagram as a complete state. UDP is unauthenticated; use a trusted LAN.
