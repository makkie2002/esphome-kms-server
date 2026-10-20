# ESPHome KMS Server (ESP32) — docker-vlmcsd equivalent

Runs a Microsoft KMS–compatible server on an ESP32 via ESPHome, listening on
TCP **1688** like `mikolatero/vlmcsd`:

```sh
docker run -d -p 1688:1688 --restart=always --name vlmcsd mikolatero/vlmcsd
```

becomes, in ESPHome:

```yaml
kms_server:
  port: 1688
```

KMS protocol v4/v5/v6, always-activated (vlmcsd `-E0` behaviour): the first
request from a client succeeds, no minimum-client counting.

> **Legal use only.** For re-activating licenses you own (e.g. after a
> motherboard/CPU change) with Microsoft-published GVLK keys. No keys ship in
> this repo. Upstream credit: Hotbird64 `vlmcsd`
> (https://github.com/Wind4/vlmcsd, archived) — C core vendored flat under
> `components/kms_server/` (see “Updating vlmcsd sources” for the few
> marked ESP32/IDF patches).

Native ESPHome — no Arduino `WiFiManager` / `WebServer` / combo firmware.
Wi-Fi provisioning is `wifi:` + `captive_portal:`, dashboard is HA
`api:` + `web_server:` + sensors, clock is `time/sntp`.

## Layout

```
kms.yaml                              # ESPHome config for HA add-on (native)
secrets.yaml.example
example-github.yaml                   # same, but pulls component from GitHub
components/
  kms_server/
    __init__.py                       # ESPHome codegen (port + intervals + sensors)
    kms_server.h / kms_server.cpp      # FreeRTOS TCP :1688 + rpcServer() + HA sensors
    kms_glue.cpp                      # ESPHome/IDF glue (replaces helpers.c/output.c:
                                      # sockets, ESP_LOG, RNG, UCS-2, KMS data loader)
    kms.c / rpc.c / crypto*.c / ...   # upstream C core, TOP LEVEL on purpose:
                                      # ESPHome only compiles top-level component files
    kms.h / rpc.h / ... / config.h    # upstream headers (+ our ESP32-minimal config.h
                                      # with FULL_INTERNAL_DATA)
```

## Use with Home Assistant ESPHome add-on

1. Copy `kms.yaml` → `/config/esphome/kms-server.yaml`
2. Copy `components/` → `/config/esphome/components/` (merge if exists)
3. Create `/config/esphome/secrets.yaml` from `secrets.yaml.example`
4. ESPHome dashboard → `kms-server` → **Install** → flash via USB on first run,
   OTA after that.
5. Find the ESP IP in HA (Settings → Devices → ESPHome) or router DHCP.
   The `web_server:` dashboard is also at `http://<ip>/`.
6. Point Windows/Office at it (same as docker-vlmcsd README):

**Windows (admin cmd):**
```bat
slmgr.vbs -upk
slmgr.vbs -ipk XXXXX-XXXXX-XXXXX-XXXXX-XXXXX
slmgr.vbs -skms <ESP-IP>:1688
slmgr.vbs -ato
slmgr.vbs -dlv
```

**Office x64 (admin cmd):**
```bat
cd \Program Files\Microsoft Office\Office16
cscript ospp.vbs /sethst:<ESP-IP>
cscript ospp.vbs /setprt:1688
cscript ospp.vbs /inpkey:xxxxx-xxxxx-xxxxx-xxxxx-xxxxx
cscript ospp.vbs /act
cscript ospp.vbs /dstatusall
```

GVLK keys: Microsoft Learn “KMS client activation and product keys”
(docs.microsoft.com → kmsclientkeys). Keys are public, not included here.

HA entities (all optional — omit any `kms_server:` sensor you don't want):

| Entity | Example |
|---|---|
| `sensor.kms_activation_count` | total since boot |
| `text_sensor.kms_last_product` | `Windows 11 Pro` (friendly name via `FULL_INTERNAL_DATA`, else GUID) |
| `text_sensor.kms_last_client` | `192.168.1.20` |
| `text_sensor.kms_last_version` | `6.3` (KMS v4/v5/v6) |
| `text_sensor.kms_last_time` | `2026-10-20 21:30` UTC, or `up+0d02h` if SNTP unsynced |

Long-term history lives in HA (Recorder/Statistics) — the ESP only keeps the
last activation, no Arduino ring buffer needed.

## How it works

- `kms_server.cpp` opens a BSD/lwIP socket (`0.0.0.0:1688`), `bind/listen/accept`
  in a FreeRTOS task (core 0, 12 KiB stack).
- Each client runs upstream `rpcServer()` (`rpc.c`): RPC bind → KMS
  request → `CreateResponseV4/V6` (`kms.c` + internal Rijndael/AES from
  `crypto_internal.c`, no OpenSSL) → RPC response.
- A `CreateResponseBase` hook (same technique as `kmserver-esp32`
  `esp_support.cpp`, no vendored files touched) resolves the product via
  `getProductIndex()` and queues product/client/version/time behind a
  `portMUX`. `loop()` publishes to HA sensors — thread-safe, no WebServer.
- `VLActivationInterval` / `VLRenewalInterval` come from YAML (defaults 120 /
  10080 min, vlmcsd `-A/-R` defaults).
- ePID/HWID: fixed defaults from `config.h` (`NO_RANDOM_EPID`, deterministic).
- RNG: `randomNumberInit()` + `srand(esp_random())` (hardware RNG).
- Logs: ESPHome logger `INFO` per activation, `DEBUG` per connect.
- Ported from `kmserver-esp32` (non-COMBO parts only): KMS core + activation
  hook + product lookup. Intentionally **not** ported: Arduino `WebServer`
  `:80` dashboard (replaced by `web_server:` + sensors), `WiFiManager`
  portal (replaced by `wifi:` + `captive_portal:`), ESP8266 backend
  (too tight for `kmsdata`), T-Dongle-S3 COMBO HID-vault/MSC/display
  (no ESPHome equivalent).

## Config options

| Key | Default | Notes |
|-----|---------|-------|
| `port` | 1688 | KMS TCP port, keep 1688 unless you know why |
| `activation_interval` | 120 | min, passed as `VLActivationInterval` |
| `renewal_interval` | 10080 | min (7 d), passed as `VLRenewalInterval` |
| `activation_count` | — | optional `sensor:` (total_increasing) |
| `last_product` / `last_client` / `last_version` / `last_time` | — | optional `text_sensor:`s |

## Troubleshooting

- **No activation:** ESP and PC on same LAN? `ping <ESP-IP>`, `telnet <ESP-IP> 1688`
  (or `Test-NetConnection -ComputerName <ESP-IP> -Port 1688`). Check ESPHome
  logs for `KMS activation served #N`.
- **ESP32 reboots / OOM:** use `esp32dev` 4 MB flash, Arduino framework (as in
  `kms.yaml`). `kmsdata` ≈ 18 KiB RAM + 12 KiB task — fine on ESP32, too tight
  for ESP8266 (ESP32 only).
- **Sensors stay `unknown`:** normal until the first client activates. Check
  `logger` shows `Listening on TCP :1688`, then run `slmgr.vbs -ato`.
- **Time shows `up+…`:** `time/sntp` not synced yet (no internet/DNS). Sensors
  still update; wall clock appears after NTP sync.
- **Office still unlicensed:** use matching GVLK (Office 2016/2019/2021 differ),
  `ospp.vbs /dstatusall` shows error code; `0xC004F038` means client never
  reached KMS (network), not an ePID problem (this build never returns it
  on purpose — always “charged”).
- **Build errors about `<pwd.h>` etc.:** all vendored files live flat at the
  component top level (ESPHome only compiles/copies top-level files) and need
  no `-I` flags. IDF lacks `<sys/shm.h>` / `<sys/ipc.h>` / `<syslog.h>`;
  those includes are skipped via `__has_include` (the code using them is
  compiled out by `config.h` anyway).

## Using from GitHub (recommended for HA add-on)

Yes. Push this folder as-is to GitHub — root must contain `components/`:

```
<repo root>/
  components/kms_server/...
  kms.yaml
  example-github.yaml
  README.md
```

Then in HA (`/config/esphome/kms-server.yaml`) use `example-github.yaml`:

```yaml
external_components:
  - source: github://YOUR_USER/YOUR_REPO@main
    components: [kms_server]
    refresh: 1d

kms_server:
  port: 1688
```

Publish steps (from `kms-esphome/`):

```sh
git init
git add .
git commit -m "ESPHome KMS server (vlmcsd port, ESP32)"
gh repo create YOUR_REPO --public --source=. --push
# or: git remote add origin git@github.com:YOUR_USER/YOUR_REPO.git && git push -u origin main
```

Then pin users to a tag: `git tag v1.0.0 && git push --tags`, and point YAML at
`github://YOUR_USER/YOUR_REPO@v1.0.0`. `refresh: 1d` re-fetches the component
daily; use the ESPHome dashboard **Clean Build Files** after changing `ref`.

Local alternative (no GitHub): keep `kms.yaml`'s `type: local` block and copy
`components/` next to your YAML.

## Updating vlmcsd sources

Vendored from `Wind4/vlmcsd@master` `src/` (only the server-needed core:
`kms`, `rpc`, `crypto*`, `endian`, `kmsdata`, `shared_globals`; no `network.c` /
`vlmcsd.c` / `vlmcs.c` / `helpers.c` / `output.c` — the last two are replaced by
`kms_glue.cpp`). To refresh: copy the same 7 `*.c` + 11 `*.h` files flat into
`components/kms_server/`, keep our `config.h`, then re-apply the marked patches:
`kms.c` + `shared_globals.h` (`__has_include` guards — IDF has no `<sys/shm.h>` /
`<sys/ipc.h>` / `<syslog.h>`), `rpc.c` (no `getnameinfo()` on lwIP, manual port
formatting — same patch as `kmserver-esp32`), `crypto_internal.c` (memcpy for
strict-alignment CPUs — same patch as `kmserver-esp32`).
