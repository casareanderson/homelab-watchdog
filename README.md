# homelab-watchdog

An independent watcher for the homelab on a **Waveshare ESP32-P4-ETH** (wired Ethernet, PoE-capable, no WiFi).
It doesn't depend on Uptime Kuma, Hermes or any server: if the boxes it watches go down, it still tells you.

## What it does

- Every 60 s it checks 10 core targets (TCP connect, plus a real DNS query to AdGuard):

  | Target | Address |
  |---|---|
  | Uptime Kuma | 192.0.2.10:3001 |
  | NAS (SSH) | 192.0.2.10:22 |
  | NPMplus | 192.0.2.10:80 |
  | Node 1 Proxmox | 192.0.2.20:8006 |
  | Node 2 Proxmox | 192.0.2.30:8006 |
  | Agents (Caddy) | 192.0.2.40:443 |
  | Password vault | 192.0.2.50:8000 |
  | Router | 192.0.2.1:80 |
  | AdGuard DNS | 192.0.2.1:3053 (A query for cloudflare.com) |
  | Internet | 1.1.1.1:443 |

- 3 failed checks in a row = a **🔴 DOWN** message in Discord; the first good check after that = **🟢 RECOVERED**
  (with how long it was down). Alerts that can't be sent (internet down) are queued (newest 20) and sent when it's back.
- Pushes to an Uptime Kuma **push monitor** every cycle, so Kuma alerts you if the watchdog itself dies.
- Status page at `http://<board-ip>/` showing every target, targets down, queued alerts, uptime, IP and MAC.
- All network checks run in their own task, so a slow or dead target never blocks the board's main loop.

## Before you flash (on your Mac)

1. **Discord webhook:** in Discord, `#alerts` → Edit Channel → Integrations → Webhooks → New Webhook → Copy Webhook URL.
2. **Kuma push monitor:** Uptime Kuma → Add New Monitor → type **Push**, name `homelab-watchdog`, heartbeat interval
   **120 s** → copy the Push URL **without** the `?status=up&msg=OK&ping=` part.
3. `cp secrets.yaml.example secrets.yaml`, then put those two URLs and a long random OTA password into `secrets.yaml`.
   Don't paste them into chat, and never commit `secrets.yaml` (it's gitignored).
   ⚠️ `esphome config` prints the webhook inside the lambda; don't share that output.

## Flash (first time over USB)

```bash
pip install esphome==2026.9.1        # or: pipx install esphome==2026.9.1
cd homelab-watchdog
esphome run homelab-watchdog.yaml    # pick the USB serial port when asked
```

Plug in the Ethernet cable (PoE or USB power both work). In the logs you should see an IP from DHCP and
`watchdog online: checking 10 targets` appear in `#alerts` within about a minute.
The first ESP-IDF build downloads a few GB of toolchain and takes a while.

**Then:** add a **DHCP reservation** on your router for the board's MAC (shown in the logs and on the status page) so it
keeps one IP. Later updates go over the network: `esphome run homelab-watchdog.yaml --device <board-ip>`.

## If something's off

- No IP / Ethernet never comes up: the PHY clock wiring may differ on your board revision. Try `clk: mode: CLK_OUT`
  (keep `pin: GPIO50`), or check the pins against Waveshare's wiki for the ESP32-P4-ETH.
- A target shows DOWN but is fine: check the address in `watchdog.h` (`TARGETS`). IPs are from 2026-10-13.
