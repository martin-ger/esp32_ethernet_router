# ESP32 Ethernet Router

Firmware for the WT32-ETH01 board that turns it into a compact network router. 

**Derived from** [esp32_nat_router](https://github.com/martin-ger/esp32_nat_router). The original project uses WiFi as the downlink (AP) and (as one option) Ethernet as the uplink. This variant reverses that: **WiFi STA is the uplink** (Internet) and **Ethernet is the downlink** (LAN).

The board connects to an existing WiFi network as a client (STA) and shares that connection with wired devices through its Ethernet port. The Ethernet side runs its own IP subnet, an optional DHCP server, and optional NAT — so Ethernet clients need no special configuration and get transparent Internet access.

```
                    upstream WiFi network
                    (hotel, office, home AP)
                            |
                     [ WiFi STA uplink ]
                      192.168.1.x / DHCP
                      or static IP
                            |
               +------------+------------+
               |     WT32-ETH01          |
               |     ESP32 Ethernet      |
               |         Router          |
               |                         |
               |  NAT / Routing          |
               |  DHCP server            |
               |  Firewall (ACL)         |
               |  WireGuard VPN client   |
               |  Packet capture (PCAP)  |
               +------------+------------+
                            |
                     [ Ethernet LAN ]
                      192.168.4.x
                      (default subnet)
                            |
              +-------------+-------------+
              |             |             |
          [PC / Server] [Lab device] [Other host]
```

All traffic from Ethernet clients is NATed through the router's WiFi uplink address by default, so the upstream network sees only a single client. NAT can be disabled for routed (non-NATed) operation if the upstream network knows the `192.168.4.0/24` subnet. A WireGuard VPN tunnel can optionally replace or supplement the direct uplink path, with a kill switch that blocks unprotected traffic when the tunnel is down.

All settings are managed through a browser-based web interface (accessible at `http://192.168.4.1` from the Ethernet LAN or via WiFi) or via the serial console at 115200 bps.

---

## Use Cases

- **Home lab gateway** — connect a wired lab segment to a WiFi network without a dedicated router
- **Travel router** — bridge hotel or shared WiFi to a private wired LAN with NAT and firewall
- **Isolated test network** — provide controlled Internet access to a bench with optional kill-switch VPN
- **Transparent monitoring tap** — capture and inspect all traffic on the wired segment in Wireshark without any client changes
- **WPA2-Enterprise bridge** — give plain devices access to a corporate WiFi that requires 802.1X authentication
- **Routed segment (no NAT)** — operate as a pure IP router, forwarding traffic between the WiFi uplink and the Ethernet segment without address translation

---

## Features

- WiFi STA uplink with automatic reconnect on disconnect
- WPA2-Personal and WPA2-Enterprise (PEAP, TTLS, TLS) on the uplink
- Configurable static IP or DHCP for the WiFi uplink
- Ethernet LAN downlink with configurable IP
- NAT (NAPT) — enable or disable per configuration; when disabled the Ethernet segment is routed
- DHCP server on Ethernet — enable or disable independently of NAT
- DHCP reservations — assign fixed IPs to clients by MAC address
- Port forwarding (DNAT) — forward external TCP/UDP ports to internal hosts
- Stateless packet firewall with four directional ACL lists, 16 rules each
- Packet capture to Wireshark over TCP (PCAP streaming, ACL-triggered or promiscuous)
- WireGuard VPN client with kill switch and split-tunnel / route-all modes
- Remote console — password-protected TCP CLI on a configurable port
- Syslog forwarding — ship ESP log output to a remote syslog server via UDP
- OTA firmware update through the web interface
- Byte counters for the WiFi uplink
- Configurable TTL override, WiFi TX power, and timezone
- All settings persisted in NVS flash; survive firmware updates

---

## Hardware — WT32-ETH01

The WT32-ETH01 is an ESP32-based module with an integrated LAN8720 Ethernet PHY. It exposes both a standard WiFi radio and a 10/100 Mbit/s Ethernet port on the same board.

| Parameter | Value |
|-----------|-------|
| SoC | ESP32 (dual-core 240 MHz) |
| Flash | 4 MB |
| Ethernet PHY | LAN8720 |
| Ethernet MDC | GPIO 23 |
| Ethernet MDIO | GPIO 18 |
| PHY address | 1 |
| PHY power | GPIO 16 |
| Status LED | GPIO 2 (configurable) |
| Serial | 115200 bps |

**LED behavior:**
- Solid on: WiFi uplink connected
- Solid off: not connected
- Blinking: active, connection state indicator

---

## Web Interface

Access the web interface from any device connected to the Ethernet LAN. The default address is `http://192.168.4.1`.

### Pages

**/ — Status**

Shows current connection state: uplink SSID, uplink IP, signal strength, Ethernet IP, VPN status (when a tunnel is configured), packet capture mode (when active), byte counters, and uptime. When a web password is set, the login form appears here.

**Configuration**

Grouped into sections. Changes trigger a reboot to apply.

- *Ethernet Subnet Settings* — LAN IP address, DNS server override, NAT toggle (enabled / disabled), DHCP server toggle (enabled / disabled)
- *WiFi Settings (Uplink)* — SSID, password, WPA2-Enterprise credentials (username, identity, EAP method, TTLS phase 2, certificate options), MAC address override
- *Static IP Settings* — static IP, subnet mask, gateway for the WiFi uplink; leave empty to use DHCP
- *Remote Console* — enable/disable, port, interface binding (ETH/STA/VPN), idle timeout
- *PCAP Packet Capture* — capture mode (off / ACL monitor / promiscuous), snaplen, live stats
- *Device Management* — OTA firmware upload, factory reset

**Mappings**

Visible only when DHCP server or NAT is enabled.

- *Connected Clients* — live DHCP lease table (shown when DHCP server is enabled)
- *DHCP Reservations* — add or remove static MAC-to-IP assignments (shown when DHCP server is enabled)
- *Port Forwarding* — add or remove TCP/UDP port forward rules (shown when NAT is enabled)

**Firewall**

Manage the four ACL lists. Add rules by selecting direction, protocol, source address/port, destination address/port, and action. Delete rules by index. Per-rule hit counters show traffic statistics.

**VPN**

WireGuard peer configuration: private key, peer public key, optional preshared key, endpoint, tunnel IP, keepalive, kill switch, and routing mode.

**WiFi Scan**

Scans for available upstream networks and displays SSID, BSSID, channel, RSSI, and authentication type.

### Password Protection

Set a password with `set_router_password <password>` or through the web interface. When set, the Configuration, Mappings, Firewall, and VPN pages require authentication. Sessions last 30 minutes. Clear the password by setting an empty string.

---

## WiFi and Network

### Uplink (WiFi STA)

```
set_sta <ssid> <password>
set_sta_static <ip> <subnet> <gateway>
set_sta_static dhcp
set_sta_mac <AA:BB:CC:DD:EE:FF>
set_tx_power <dBm>
```

The router connects to the upstream WiFi network and reconnects immediately on disconnect. WPA2-Enterprise (PEAP/TTLS/TLS) can be configured in the web interface.

### Ethernet Downlink

```
set_ap_ip <ip>
set_ap_dns <dns>
set_eth_nat <on|off>
set_eth_dhcps <on|off>
```

The Ethernet interface defaults to `192.168.4.1/24` with NAT and DHCP server enabled. Changes to NAT or DHCP server state take effect after reboot.

When NAT is disabled, the Ethernet segment is routed — clients use their own IP addresses, and the router forwards packets to the default gateway (WiFi uplink or VPN tunnel).

When DHCP server is disabled, clients must be configured with static IPs in the `192.168.4.x` range (or whatever subnet you set).

### DHCP Reservations

```
dhcp_reserve add <AA:BB:CC:DD:EE:FF> <ip> [-n <name>]
dhcp_reserve del <AA:BB:CC:DD:EE:FF>
```

Reservations persist in NVS and are shown in the Mappings page. Up to 16 reservations are supported.

### Port Forwarding

```
portmap add TCP <external_port> <internal_ip> <internal_port>
portmap add UDP <external_port> <internal_ip> <internal_port>
portmap del TCP <external_port>
portmap del UDP <external_port>
```

Only available when NAT is enabled. Rules persist in NVS.

### Other Network Settings

```
set_hostname <name>
set_ttl <value>
set_tz <POSIX TZ string>
set_tz clear
```

`set_ttl` overrides the TTL field in forwarded packets, which can help with tethering restrictions. `set_tz` accepts a POSIX timezone string (e.g. `CET-1CEST,M3.5.0,M10.5.0/3`).

---

## Firewall

The firewall filters packets at four points in the data path:

| List | Direction | Description |
|------|-----------|-------------|
| `to_esp` | inbound on WiFi uplink | packets arriving from the Internet |
| `from_esp` | outbound on WiFi uplink | packets leaving to the Internet |
| `from_eth` | inbound on Ethernet | packets arriving from LAN clients |
| `to_eth` | outbound on Ethernet | packets going to LAN clients |

Rules are evaluated in order; the first match wins. Unmatched packets are allowed by default.

### Rule Syntax (CLI)

```
acl <list> <proto> <src> [<s_port>] <dst> [<d_port>] <action>
acl <list> del <index>
acl <list> clear
acl <list> clear_stats
acl show [<list>]
```

**proto:** `IP`, `TCP`, `UDP`, `ICMP`

**address:** CIDR notation (`192.168.0.0/24`), single IP, or `any`; named DHCP reservations can be used by device name

**port:** port number or `*` for any (TCP/UDP only)

**action:** `allow`, `deny`, `allow_monitor`, `deny_monitor`

The `_monitor` variants also send the matching packet to the PCAP capture stream, regardless of the global capture mode setting.

### Example

Drop all inbound TCP to the router's web port and block LAN clients from reaching the router management interface:

```
acl to_esp TCP any * any 80 deny
acl from_eth TCP any * 192.168.4.1 80 deny
```

Rules are stored in NVS under keys `acl_0` through `acl_3`.

---

## Packet Capture

Traffic on the Ethernet interface can be streamed live to Wireshark over a TCP connection on port 19000. No client software other than netcat and Wireshark is required.

### Capture Modes

| Mode | Behavior |
|------|----------|
| `off` | No capture |
| `acl` | Capture only packets matched by a `_monitor` ACL rule |
| `promisc` | Capture all Ethernet interface traffic |

### Usage

```
pcap <off|acl|promisc>
pcap snaplen [<bytes>]
pcap status
```

Connect from a workstation on the LAN:

```
nc 192.168.4.1 19000 | wireshark -k -i -
```

The connection command is also shown in the PCAP section of the Configuration page. Snaplen limits the captured bytes per packet (64–1600, default 1600).

---

## WireGuard VPN

The router includes a WireGuard client that establishes a tunnel over the WiFi uplink. The VPN tunnel can be used as the default gateway for all Ethernet clients.

### Configuration (CLI)

```
set_vpn private_key <base64 key>
set_vpn public_key <base64 key>
set_vpn preshared_key <base64 key>
set_vpn endpoint <host>
set_vpn port <udp_port>
set_vpn address <tunnel_ip>
set_vpn netmask <netmask>
set_vpn keepalive <seconds>
set_vpn killswitch <on|off>
set_vpn route_all <on|off>
set_vpn <on|off>
```

### Options

**Kill switch** (`killswitch on`): blocks Ethernet client Internet traffic when the VPN tunnel is down, preventing unprotected traffic from leaking through the plain WiFi uplink.

**Route all** (`route_all on`): sends all client traffic through the VPN tunnel. When disabled (split-tunnel), only traffic destined for the VPN peer subnet is routed through the tunnel; all other traffic uses the WiFi uplink directly.

VPN status (connected / disconnected) is shown on the index page when a tunnel is configured.

---

## Remote Console

A TCP server provides a password-authenticated CLI session accessible over the network. It reuses the web interface password. Output from CLI commands is captured and forwarded to the remote session.

```
remote_console enable
remote_console disable
remote_console port <port>
remote_console bind <eth|sta|vpn|both>
remote_console timeout <seconds>
remote_console kick
remote_console status
```

Default port is 2323. Connect with any TCP client:

```
nc 192.168.4.1 2323
```

The service is disabled by default. A web password must be set before enabling it. Idle sessions are disconnected after the configured timeout (default 300 seconds; 0 disables the timeout). Only one session is active at a time.

The `bind` option controls which network interfaces the server listens on (ETH = Ethernet downlink, STA = WiFi uplink, VPN = WireGuard tunnel).

---

## Syslog

ESP log output can be forwarded to a remote syslog server over UDP.

```
syslog enable <server> [<port>]
syslog disable
syslog status
```

The default port is 514. The server address is resolved by DNS and re-resolved each time the WiFi uplink connects. Configuration is persisted in NVS.

---

## CLI Reference

Connect via serial at 115200 bps, or via the remote console.

### Network

| Command | Description |
|---------|-------------|
| `set_sta <ssid> <password>` | Set upstream WiFi credentials |
| `set_sta_static <ip> <mask> <gw>` | Set static IP for WiFi uplink |
| `set_sta_static dhcp` | Revert WiFi uplink to DHCP |
| `set_sta_mac <mac>` | Override WiFi MAC address |
| `set_ap_ip <ip>` | Set Ethernet interface IP |
| `set_ap_dns <dns>` | Set DNS server for Ethernet clients |
| `set_eth_nat <on\|off>` | Enable or disable NAT (requires reboot) |
| `set_eth_dhcps <on\|off>` | Enable or disable DHCP server (requires reboot) |
| `set_hostname <name>` | Set DHCP hostname |
| `set_ttl <value>` | Override TTL in forwarded packets |
| `set_tx_power <dBm>` | Set WiFi transmit power |
| `set_tz <TZ string>` | Set POSIX timezone |
| `bytes` | Show uplink byte counters |
| `bytes reset` | Reset byte counters |

### DHCP and Port Mapping

| Command | Description |
|---------|-------------|
| `dhcp_reserve add <mac> <ip> [-n <name>]` | Add DHCP reservation |
| `dhcp_reserve del <mac>` | Remove DHCP reservation |
| `portmap add <TCP\|UDP> <ext_port> <int_ip> <int_port>` | Add port forward rule |
| `portmap del <TCP\|UDP> <ext_port>` | Remove port forward rule |

### Firewall

| Command | Description |
|---------|-------------|
| `acl show [<list>]` | Show rules and hit counts |
| `acl <list> <proto> <src> [<sp>] <dst> [<dp>] <action>` | Add rule |
| `acl <list> del <index>` | Delete rule by index |
| `acl <list> clear` | Remove all rules from list |
| `acl <list> clear_stats` | Reset hit counters |

Lists: `to_esp`, `from_esp`, `from_eth`, `to_eth`

### VPN

| Command | Description |
|---------|-------------|
| `set_vpn <on\|off>` | Enable or disable VPN tunnel |
| `set_vpn private_key <key>` | WireGuard private key (base64) |
| `set_vpn public_key <key>` | Peer public key (base64) |
| `set_vpn preshared_key <key>` | Preshared key (base64, optional) |
| `set_vpn endpoint <host>` | Peer endpoint address |
| `set_vpn port <port>` | Peer UDP port (default 51820) |
| `set_vpn address <ip>` | Tunnel IP address |
| `set_vpn netmask <mask>` | Tunnel subnet mask |
| `set_vpn keepalive <seconds>` | Persistent keepalive interval |
| `set_vpn killswitch <on\|off>` | Block traffic when VPN is down |
| `set_vpn route_all <on\|off>` | Route all traffic through VPN |

### Packet Capture

| Command | Description |
|---------|-------------|
| `pcap <off\|acl\|promisc>` | Set capture mode |
| `pcap snaplen [<bytes>]` | Get or set max bytes per packet |
| `pcap status` | Show capture statistics |

### Remote Console and Syslog

| Command | Description |
|---------|-------------|
| `remote_console enable` | Enable remote console |
| `remote_console disable` | Disable remote console |
| `remote_console port <port>` | Set TCP port |
| `remote_console bind <eth\|sta\|vpn\|both>` | Set interface binding |
| `remote_console timeout <seconds>` | Set idle timeout |
| `remote_console kick` | Disconnect active session |
| `remote_console status` | Show status |
| `syslog enable <server> [<port>]` | Enable syslog forwarding |
| `syslog disable` | Disable syslog forwarding |
| `syslog status` | Show syslog configuration |

### Web Interface

| Command | Description |
|---------|-------------|
| `web_ui enable` | Enable web server (after reboot) |
| `web_ui disable` | Disable web server (after reboot) |
| `web_ui port <port>` | Set web server port (default 80) |
| `set_router_password <password>` | Set web interface password |

### Status and System

| Command | Description |
|---------|-------------|
| `show status` | Connection state, IPs, heap |
| `show config` | WiFi and Ethernet configuration |
| `show mappings` | DHCP pool, reservations, port maps |
| `scan` | Scan for WiFi networks |
| `factory_reset` | Erase all NVS settings and reboot |

---

## Building

Requires ESP-IDF v5.x. Source the ESP-IDF environment, then run the provided build script:

```bash
. $IDF_PATH/export.sh
./build_firmware.sh
```

The script performs a clean build and copies the four binary files into the `firmware/` directory:

```
firmware/
├── bootloader.bin
├── partition-table.bin
├── ota_data_initial.bin
└── esp32_eth_router.bin
```

To reconfigure build options before building:

```bash
idf.py -B build_eth_sta menuconfig
```

The `sdkconfig.defaults.wt32_eth_sta_uplink` file sets the Ethernet PHY GPIOs for the WT32-ETH01 (MDC=23, MDIO=18, PHY addr=1, PHY power=16). The base `sdkconfig.defaults` enables IP forwarding, NAPT, and the custom DHCP server with up to 16 leases.

OTA updates are also supported through the web interface (Device Management section) with partition rollback on failed updates.

---

## Installation

Flash the binaries from the `firmware/` directory using `esptool.py`:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write_flash \
  0x1000  firmware/bootloader.bin \
  0x8000  firmware/partition-table.bin \
  0xf000  firmware/ota_data_initial.bin \
  0x20000 firmware/esp32_eth_router.bin
```

After flashing, connect via serial at 115200 bps and configure the upstream WiFi:

```
set_sta MyWiFiSSID MyPassword
```

The router will reboot and connect. Access the web interface at `http://192.168.4.1` from a device connected to the Ethernet port or via WiFi at the assigned IP.

To erase all settings and return to defaults:

```
factory_reset
```

or via esptool to wipe the entire flash:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash
```


## Licence

The WireGuard submodul has the following licence_
```
Copyright (c) 2021 Kenta Ida (fuga@fugafuga.org)

The original license is below:
Copyright (c) 2021 Daniel Hope (www.floorsense.nz)
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.
* Neither the name of "Floorsense Ltd", "Agile Workspace Ltd" nor the names of
  its contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Author: Daniel Hope <daniel.hope@smartalock.com>
```
