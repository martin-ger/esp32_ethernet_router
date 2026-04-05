# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32 Ethernet Router - Firmware where **WiFi STA is the uplink** (Internet) and **Ethernet is the downlink** (LAN). This is the inverse of the original ESP32 NAT Router topology. Supports NAT routing, DHCP server, VPN/WireGuard, ACL firewall, DHCP reservations, and port mapping.

**Supported hardware variants:**
- **WT32-ETH01** — ESP32 (dual-core, 240 MHz) with built-in LAN8720 Ethernet PHY
- **W5500 + ESP32-C3 SuperMini** — ESP32-C3 (single-core RISC-V, 160 MHz) with W5500 SPI Ethernet module

Both variants share all router logic. The only divergence is Ethernet MAC/PHY initialization, selected at build time via Kconfig (`CONFIG_ETH_DOWNLINK_EMAC` vs `CONFIG_ETH_DOWNLINK_W5500`).

## Build Commands

### WT32-ETH01 (ESP32 + LAN8720)
```bash
./build_firmware.sh               # Clean build → firmware/
idf.py -B build_eth_sta menuconfig
idf.py -B build_eth_sta build
idf.py -B build_eth_sta flash monitor   # 115200 bps
```

### W5500 + ESP32-C3
```bash
./build_firmware_w5500_c3.sh      # Clean build → firmware_w5500_c3/
idf.py -B build_w5500_c3 menuconfig
idf.py -B build_w5500_c3 \
  -D SDKCONFIG=sdkconfig.w5500_c3 \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.w5500_c3" \
  build
idf.py -B build_w5500_c3 -p /dev/ttyACM0 flash monitor   # SuperMini (USB-JTAG)
```

**Note:** Each variant uses a separate build directory and sdkconfig file to avoid conflicts. Always pass `-D SDKCONFIG=sdkconfig.w5500_c3` for the W5500 build or it will overwrite the default `sdkconfig`.

## Architecture

### Network Topology
```
                          ESP32 Ethernet Router (WT32-ETH01)
                    ┌─────────────────────────────────────┐
                    │                                     │
Internet ──────────►│  WiFi STA (uplink)   Ethernet (LAN)│◄──── Clients
         ◄──────────│                                     │─────►
                    └─────────────────────────────────────┘
```

- **WiFi STA**: Connects to upstream AP for Internet access
- **Ethernet**: LAN interface with optional DHCP server and NAT

### Source Structure
```
main/
├── esp32_nat_router.c   # Entry point: WiFi+ETH init, event handling, LED status
├── dhcp_manager.c       # DHCP reservation management (add/del/lookup/print)
├── netif_hooks.c        # Network interface hooks for byte counting and ACL
├── portmap.c            # Port mapping table management
└── vpn_manager.c        # VPN/WireGuard configuration and management

include/
├── router_globals.h     # Global variables and shared state
├── router_config.h      # NVS namespace and config constants
├── dhcp_reservations.h  # DHCP reservation API
├── portmap.h            # Port mapping API
├── vpn_config.h         # VPN configuration structures
├── web_password.h       # Web password hashing API
└── wifi_config.h        # WiFi config parameter helpers

components/
├── acl/                 # Stateless packet filtering firewall (4 ACL lists, 16 rules each)
├── dhcpserver/          # Custom DHCP server with reservation support (overrides ESP-IDF built-in)
├── http_server/         # Web UI server (pages: /, /config, /mappings, /firewall, /vpn, /scan)
├── pcap_capture/        # PCAP packet capture with TCP streaming to Wireshark
├── remote_console/      # Network-accessible CLI via TCP (password protected)
├── cmd_router/          # CLI commands: set_sta, set_ap_ip, set_ap_dns, set_eth_nat, set_eth_dhcps, portmap, dhcp_reserve, web_ui, set_router_password, show, acl, remote_console, syslog
└── cmd_system/          # System commands: free, heap, restart, factory_reset, tasks
```

### Custom DHCP Server Component
The `components/dhcpserver/` directory contains a custom DHCP server implementation that overrides the ESP-IDF built-in version using linker wrapping (`--wrap`).

**Structure:**
```
components/dhcpserver/
├── CMakeLists.txt                 # Build config with --wrap linker flags
├── dhcpserver.c                   # Implementation (functions use __wrap_ prefix)
└── include/dhcpserver/
    ├── dhcpserver.h               # Public API
    └── dhcpserver_options.h       # DHCP option definitions
```

**How it works:**
- `CONFIG_LWIP_DHCPS` remains enabled (ESP-IDF config options work normally)
- Linker `--wrap=<func>` redirects all calls to `__wrap_<func>` implementations
- Original ESP-IDF functions available via `__real_<func>()` if needed

**Wrapped functions:**
- `dhcps_new`, `dhcps_delete`, `dhcps_start`, `dhcps_stop`
- `dhcps_option_info`, `dhcps_set_option_info`
- `dhcp_search_ip_on_mac`, `dhcps_set_new_lease_cb`
- `dhcps_dns_setserver`, `dhcps_dns_getserver` (and `_by_type` variants)

**To modify DHCP behavior:** Edit `components/dhcpserver/dhcpserver.c`

### Key Global Variables (in router_globals.h / router_config.h)
- `ssid`, `passwd` - Upstream WiFi credentials (STA)
- `static_ip`, `subnet_mask`, `gateway_addr` - Static IP config for STA
- `my_ip`, `my_ap_ip` - Current IP addresses (STA uplink, ETH downlink)
- `eth_nat_enabled` - Whether NAT is active on Ethernet (default: 1)
- `eth_dhcps_enabled` - Whether DHCP server is active on Ethernet (default: 1)
- `portmap_tab[]` - Port mapping table (max `IP_PORTMAP_MAX` entries)
- `dhcp_reservations[]` - DHCP reservation table (max 16 entries)
- `ap_connect` - STA connection status flag

### Ethernet NAT and DHCP Server Toggles
Both NAT and DHCP server on the Ethernet interface are runtime-configurable and persisted in NVS.

**Globals** (`router_config.h` extern declarations):
```c
extern uint8_t eth_nat_enabled;    // 1=NAT active, 0=routed mode
extern uint8_t eth_dhcps_enabled;  // 1=DHCP server active, 0=disabled
```

**Behavior:**
- `eth_nat_enabled=0`: Ethernet traffic is routed (not NATed) to the default gateway (STA uplink or VPN)
- `eth_nat_enabled=1`: Standard NAT — Ethernet clients share the STA's IP address
- `eth_dhcps_enabled=0`: No DHCP server on Ethernet; clients must use static IPs
- `eth_dhcps_enabled=1`: DHCP server assigns IPs to Ethernet clients
- **Note:** DHCP server state is applied at netif creation time (requires reboot to change)

**NVS keys** (namespace `esp32_nat`):
- `eth_nat` (int) - NAT enabled flag (default 1)
- `eth_dhcps` (int) - DHCP server enabled flag (default 1)

### DHCP Reservations
The custom DHCP server supports IP reservations - assigning fixed IPs to specific MAC addresses.

**Data structure** (`router_globals.h`):
```c
struct dhcp_reservation_entry {
    uint8_t mac[6];                              // Client MAC address
    uint32_t ip;                                 // Reserved IP address
    char name[DHCP_RESERVATION_NAME_LEN];        // Optional device name (32 chars)
    uint8_t valid;                               // Entry active flag
};
```

**Functions** (`dhcp_manager.c`):
- `add_dhcp_reservation(mac, ip, name)` - Add/update reservation
- `del_dhcp_reservation(mac)` - Remove reservation by MAC
- `lookup_dhcp_reservation(mac)` - Get reserved IP for MAC (returns 0 if none)
- `print_dhcp_reservations()` - Print all reservations to console

**Storage:** Persisted in NVS as blob under key `"dhcp_res"`

**Note:** MAC blocking (IP=0 reservations) has been removed — it is not effective on Ethernet since wired clients can bypass DHCP with static IPs.

### ACL Firewall Component
The `components/acl/` directory implements a stateless packet filtering firewall with four ACL lists.

**Structure:**
```
components/acl/
├── CMakeLists.txt
├── acl.c                    # Implementation: packet matching, rule management
└── include/acl.h            # Public API and data structures
```

**Network topology and ACL naming:**
```
                          ESP32 Ethernet Router
                    ┌───────────────────────┐
                    │                       │
Internet ──to_esp──►│  STA (WiFi)  ETH (LAN)│◄──from_eth─── Clients
         ◄─from_esp─│                       │───to_eth──►
                    └───────────────────────┘
```

**ACL Lists** (defined in `acl.h`):
| Index | Name | Direction | Description |
|-------|------|-----------|-------------|
| 0 | `to_esp` | Uplink inbound | Internet → ESP32 (via WiFi STA) |
| 1 | `from_esp` | Uplink outbound | ESP32 → Internet (via WiFi STA) |
| 2 | `from_eth` | ETH inbound | Clients → ESP32 (via Ethernet) |
| 3 | `to_eth` | ETH outbound | ESP32 → Clients (via Ethernet) |

**Data structure** (`acl.h`):
```c
typedef struct {
    uint32_t src;        // Source IP (pre-masked, network byte order)
    uint32_t s_mask;     // Source subnet mask
    uint32_t dest;       // Destination IP (pre-masked)
    uint32_t d_mask;     // Destination subnet mask
    uint16_t s_port;     // Source port (0 = any, TCP/UDP only)
    uint16_t d_port;     // Destination port (0 = any)
    uint8_t proto;       // Protocol: 0=any, 1=ICMP, 6=TCP, 17=UDP
    uint8_t allow;       // Action: ACL_DENY, ACL_ALLOW, or with ACL_MONITOR
    uint32_t hit_count;  // Packets matched by this rule
    uint8_t valid;       // Entry is active
} acl_entry_t;
```

**Action codes:**
- `ACL_DENY` (0x00) - Drop packet
- `ACL_ALLOW` (0x01) - Allow packet
- `ACL_MONITOR` (0x02) - Flag: also capture to PCAP
- `ACL_NO_MATCH` (0xFF) - No rule matched (packet allowed by default)

**Key functions** (`acl.c`):
- `acl_init()` - Initialize ACL subsystem
- `acl_add(acl_no, src, s_mask, dest, d_mask, proto, s_port, d_port, allow)` - Add rule
- `acl_delete(acl_no, rule_idx)` - Delete rule by index (compacts list)
- `acl_check_packet(acl_no, pbuf)` - Check packet against ACL, returns action
- `acl_clear(acl_no)` - Clear all rules from list
- `acl_parse_ip(str, &ip, &mask)` - Parse "192.168.1.0/24" or "any"
- `acl_format_ip(ip, mask, buf, len)` - Format IP with CIDR notation

**Rule processing:**
- Rules evaluated in order (first match wins)
- No match = packet allowed (permissive default)
- Non-IPv4 traffic (ARP, IPv6) passes without filtering
- Port filters only match TCP/UDP packets

**Storage:** Persisted in NVS as blobs under keys `"acl_0"` through `"acl_3"`

**Integration points:**
- Hooks called from `netif_input_hook()` and `netif_linkoutput_hook()` in `esp32_nat_router.c`
- Web interface at `/firewall` for rule management
- CLI commands via `cmd_router.c`

### Remote Console Component
The `components/remote_console/` directory provides network-accessible CLI via TCP.

**Structure:**
```
components/remote_console/
├── CMakeLists.txt           # Build config with linker wrapping for printf capture
├── include/remote_console.h # Public API
└── remote_console.c         # TCP server, authentication, session management
```

**Features:**
- TCP server on port 2323 (configurable)
- Password authentication (reuses `web_password` from NVS)
- Single concurrent session with idle timeout
- Output capture via linker wrapping (`--wrap=printf`, `--wrap=puts`, etc.)
- Disabled by default for security

**Key functions:**
- `remote_console_init()` - Initialize and start if enabled
- `remote_console_enable()` / `remote_console_disable()` - Control service
- `remote_console_kick()` - Disconnect current session
- `remote_console_get_status()` - Get connection status

**Output capture mechanism:**
Uses linker `--wrap` to intercept output functions:
- `__wrap_printf`, `__wrap_puts`, `__wrap_putchar`
- `__wrap_fputs`, `__wrap_fwrite`
- Only captures during command execution (`rc_capturing` flag)
- Does NOT wrap `vprintf` (used by ESP_LOG internally)

**NVS keys:**
- `rc_enabled` (u8) - Service enabled flag
- `rc_port` (u16) - TCP port (default 2323)
- `rc_bind` (u8) - Interface binding (0=both, 1=ETH, 2=STA)
- `rc_timeout` (u32) - Idle timeout in seconds

**CLI commands:**
```
remote_console status         # Show status
remote_console enable         # Enable service
remote_console disable        # Disable service
remote_console port <port>    # Set TCP port
remote_console bind <eth|sta|both>
remote_console timeout <sec>  # Set idle timeout
remote_console kick           # Disconnect session
```

### Configuration Storage
All settings persist in NVS (Non-Volatile Storage) under namespace `esp32_nat`:
- WiFi STA credentials, static IP settings, port mappings, DHCP reservations
- Web interface password (`web_password` key) and disable state (`lock` key)
- ETH NAT state (`eth_nat` key), ETH DHCP server state (`eth_dhcps` key)
- Survives firmware updates (use `esptool.py erase_flash` for factory reset)

### Critical SDK Configuration
These settings in `sdkconfig.defaults` are required:
```
CONFIG_LWIP_IP_FORWARD=y    # Enable IP forwarding
CONFIG_LWIP_IPV4_NAPT=y     # Enable NAT (code won't compile without this)
CONFIG_LWIP_L2_TO_L3_COPY=y # LWIP layer support
```

## Serial Console Commands

Connect at 115200 bps. Key commands:
```
set_sta <ssid> <passwd>                          # Set upstream WiFi credentials
set_sta_static <ip> <subnet> <gw>               # Static IP for STA uplink
set_sta_static dhcp                              # Revert STA to DHCP (requires restart)
set_ap_ip <ip>                                   # Set IP for the Ethernet downlink interface
set_ap_dns <dns>                                 # Set DNS server for Ethernet clients
set_eth_nat <on|off>                             # Enable/disable NAT on Ethernet (requires restart)
set_eth_dhcps <on|off>                           # Enable/disable DHCP server on Ethernet (requires restart)
portmap add TCP <ext_port> <int_ip> <int_port>   # Add port mapping (only when NAT enabled)
portmap del TCP <ext_port>                       # Delete port mapping
dhcp_reserve add <mac> <ip> [-n <name>]          # Add DHCP reservation (only when DHCP server enabled)
dhcp_reserve del <mac>                           # Delete DHCP reservation
set_web_password <password>                      # Set web interface password (empty to disable)
web_ui enable                                    # Enable web interface (after reboot)
web_ui disable                                   # Disable web interface (after reboot)
web_ui port <port>                               # Set web server port, default 80 (after reboot)
show status                                      # Show router status (connection, IPs, memory)
show config                                      # Show router configuration (WiFi/ETH settings)
show mappings                                    # Show DHCP pool, reservations and port mappings
factory_reset                                    # Erase all settings and restart (factory reset)
acl show [<list>]                                # Show ACL rules and statistics
acl add <list> <proto> <src> <sport> <dst> <dport> <action>  # Add ACL rule
acl del <list> <index>                           # Delete ACL rule by index
acl clear <list>                                 # Clear all rules from ACL list
remote_console status                            # Show remote console status
remote_console enable                            # Enable remote console (requires web_password)
remote_console disable                           # Disable remote console
```

**Removed commands** (not applicable to this variant):
- `set_ap` / `set_ap_mac` — no WiFi AP in this variant

## Web Interface

Access at the Ethernet downlink IP (default `http://192.168.4.1`) from a device connected via Ethernet.

**Pages:**
- `/` - System status (Uplink SSID, Uplink IP, Uplink Signal, Ethernet IP, VPN Status, Monitoring, Bytes, Uptime)
- `/config` - Router configuration (WiFi uplink settings, Ethernet subnet settings incl. NAT+DHCP toggles) - protected
- `/mappings` - DHCP reservations and port forwarding - protected; sections shown conditionally:
  - "Connected Clients" and "DHCP Reservations" only shown when `eth_dhcps_enabled`
  - "Port Forwarding" only shown when `eth_nat_enabled`
- `/firewall` - ACL firewall rules management (4 lists, add/delete rules, view stats) - protected
- `/vpn` - WireGuard VPN configuration - protected
- `/scan` - WiFi scan for upstream networks

**Navigation button visibility:**
- "Mappings" button on index page only shown when `eth_dhcps_enabled || eth_nat_enabled`

**Password Protection:**
- Optional password protects `/config`, `/mappings`, `/firewall`, `/vpn` pages
- Login form shown on index page when password is set
- Cookie-based sessions with 30-minute timeout
- Set via web interface or `set_web_password` command
- Empty password disables protection

**Session Management** (`http_server.c`):
- `create_session()` - Generate token and set cookie
- `is_authenticated()` - Validate session cookie and expiry
- `clear_session()` - Logout / invalidate session
- Session state stored in static variables (lost on reboot)

### Network Interface Hooks (Byte Counting)
The firmware hooks into the lwIP network interface to count bytes transmitted/received on the STA (WiFi uplink) interface.

**How it works** (`esp32_nat_router.c`):
- Uses `esp_netif_get_netif_impl()` to access the underlying lwIP `struct netif`
- Saves original function pointers and installs custom hooks
- Hooks intercept packets, count bytes, then call original functions

**STA Interface Hooks (active):**
```c
static netif_input_fn original_netif_input;
static netif_linkoutput_fn original_netif_linkoutput;

netif_input_hook(pbuf, netif)      // Counts sta_bytes_received
netif_linkoutput_hook(netif, pbuf) // Counts sta_bytes_sent
```

**ETH Interface Hooks (scaffolding for future use):**
```c
static netif_input_fn original_ap_netif_input;
static netif_linkoutput_fn original_ap_netif_linkoutput;

ap_netif_input_hook(pbuf, netif)      // Currently pass-through
ap_netif_linkoutput_hook(netif, pbuf) // Currently pass-through
```

**Public API** (`router_globals.h`):
- `init_byte_counter()` - Install STA hooks (called after WiFi init)
- `init_ap_netif_hooks()` - Install ETH hooks (called during app_main)
- `get_sta_bytes_sent()` - Get total bytes sent via STA
- `get_sta_bytes_received()` - Get total bytes received via STA
- `reset_sta_byte_counts()` - Reset counters to zero

**Global counters:**
- `sta_bytes_sent` (uint64_t) - Total bytes sent through STA (WiFi uplink)
- `sta_bytes_received` (uint64_t) - Total bytes received through STA (WiFi uplink)

**Note:** Uses internal ESP-IDF API (`esp_netif_get_netif_impl`) which may change between versions.

## WiFi Reconnect Behavior
- No exponential backoff — immediate reconnect on disconnect
- `sta_reconnect()` calls `esp_wifi_connect()` directly
- Reconnect is paused during WiFi scanning

## LED Status
- Solid on: Connected to upstream WiFi AP
- Solid off: Not connected
- Blinking: Active (blink pattern indicates connection state)

LED is off (-1) by default.
