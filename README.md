# SubBridge

[![Build and Release](https://github.com/Murchiz/Subs-converter/actions/workflows/build.yml/badge.svg)](https://github.com/Murchiz/Subs-converter/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/Language-C%2B%2B23-00599C.svg)](https://en.cppreference.com/w/cpp/23)

**SubBridge** is a high-performance, lightweight local proxy subscription converter, aggregator, and bridge written in C++ for Windows and Linux.

It parses and normalizes diverse proxy subscription formats and protocols (VLESS, VMess, Trojan, Shadowsocks, Hysteria 1/2, TUIC, Clash YAML, Sing-box JSON, V2Ray Base64) with zero external heavy dependencies, low memory footprint, and sub-millisecond conversion latency.

---

## ✨ Features

- **Multi-Protocol & Multi-Format Parsing**:
  - Protocols: `vless://`, `vmess://`, `trojan://`, `ss://`, `hysteria://`, `hysteria2://`, `tuic://`, `wireguard://`
  - Targets:
    - `clash`: Clash / Mihomo YAML
    - `singbox-android`: Sing-box JSON (Mobile / Android)
    - `singbox-pc`: Sing-box JSON (Desktop / PC with SOCKS & Mixed inbounds)
    - `xray`: Base64-encoded V2Ray / Xray link list
    - `xray-one`: Single consolidated Xray JSON configuration
    - `xray-jsons`: Array of standalone Xray JSON configurations (`[...]`) containing balancers with `burstObservatory` and individual node configs
    - `v2ray`: Base64-encoded V2Ray link list
  - Full routing rule extraction and conversion between Xray/Sing-box rules and Clash rule sets.
- **Smart Balancer Handling & Deduplication**:
  - Automatically identifies upstream subscription balancers (e.g. leastPing urltest groups).
  - `xray-jsons` preserves original Xray balancer configs intact, including strategy settings (such as `leastLoad`), selectors, routing, fallback and observatory settings. Rules from other aggregated subscriptions do not overwrite those configs.
  - Eliminates duplicate node tags (e.g. `proxy-2`) by mapping balancer members to their respective proxy nodes.
  - Intelligently omits redundant auto-balancer groups if upstream balancers already cover all nodes.
  - Optional `force_balancer` flag to force SubBridge's generic `Auto` group alongside upstream balancers.
- **Subscription Merging**: Aggregate up to 8 upstream subscription links per route into a single unified endpoint.
- **Auto-Spawned Conversion Ports**: Specify `converts = clash, singbox-android, singbox-pc, xray-jsons` in `config.ini` to automatically spin up dedicated converted endpoints on consecutive local ports.
- **Subconverter Compatible API**: Built-in HTTP endpoint on port `25500` compatible with standard subconverter clients (`http://127.0.0.1:25500/sub?target=clash&url=...`).
- **Device Fingerprinting & Spoofing**: Automatic hardware ID (`MachineGuid` / `machine-id`) and OS telemetry gathering with optional per-route spoofing and custom `User-Agent` headers.
- **Flexible Execution Modes**:
  - Run interactively in terminal (`--console`)
  - Run in the background as a Windows Service or Linux systemd daemon (`--install`, `--uninstall`, `--restart`)
  - Static one-shot file converter via CLI (`--convert`)
  - Built-in help and version flags (`--help`, `-h`, `--version`, `-v`)

---

## 🛠️ Building from Source

### Prerequisites
- **CMake** (3.20 or newer)
- **Ninja** (recommended)
- **C++23 Compiler**:
  - **Windows**: Visual Studio 2022 / MSVC
  - **Linux**: GCC 14+ or Clang with a C++23-capable standard library (including `<format>` and `<print>`)
- **Linux Build Dependencies**:
  - `libcurl` development package (e.g. `sudo apt install -y libcurl4-openssl-dev ninja-build`)
- **Python 3** (optional, for YAML tests)

CMake requires C++23 with compiler extensions disabled and checks the required standard-library features at configure time. Builds use strict conformance diagnostics (`-pedantic-errors` on GCC/Clang, `/permissive-` on MSVC). Native tests keep assertions enabled in all build configurations, including Release/CI.

### Build with CMake Presets (Recommended)

Configure, compile with full optimizations, and run tests:

```bash
# 1. Configure optimized release preset
cmake --preset release

# 2. Build binaries
cmake --build --preset release

# 3. Run test suite
ctest --preset release
```

The optimized executable will be located in `build/release/sub_bridge.exe` (Windows) or `build/release/sub_bridge` (Linux).

---

## ⚙️ Configuration

SubBridge looks for `config.ini` in the same directory as the executable. Copy [`config.ini.example`](config.ini.example) to get started:

```bash
# Windows
Copy-Item config.ini.example config.ini

# Linux
cp config.ini.example config.ini
```

### Configuration Structure

```ini
# [Device] (Optional hardware telemetry override)
[Device]
# hwid = 00000000-0000-0000-0000-000000000000
# os = Windows
# ver = 10.0.22631
# model = Custom PC

# [Sub_<Name>] (Subscription group)
[Sub_Main]
name = MyProxyBundle
port = 25501
hwid = false

# Upstream subscription links (link1 .. link8)
link1 = https://example.com/api/v1/client/subscribe?token=xxx
user_agent1 = Happ/3.23.0

# Force SubBridge generic Auto balancer group even if upstream already provides auto-balancers (default: false)
force_balancer = false

# Automatically expose converted outputs on consecutive ports
# Port 25501 -> Raw / Merged V2Ray Base64
# Port 25502 -> Clash / Mihomo YAML
# Port 25503 -> Sing-Box Android JSON
# Port 25504 -> Sing-Box PC JSON
# Port 25505 -> Xray / V2Ray Share Links (Base64)
# Port 25506 -> Xray Standalone Config JSON
# Port 25507 -> Xray Configs Array JSON ([...])
converts = clash, singbox-android, singbox-pc, xray, xray-one, xray-jsons

# Optional: remove upstream Happ / INCY app-management parameters
# strip_happ = true
# strip_incy = true
```

### Happ and INCY subscription parameters

SubBridge can remove or add Happ ([app management](https://www.happ.su/main/dev-docs/app-management)) and INCY ([app management](https://github.com/INCY-DEV/incy-docs/blob/main/ru/dev-docs/app-management.en.md)) parameters for each `[Sub_*]` route.

- **Strip:** `strip_happ = true` or `strip_incy = true` removes that app's upstream directives from headers and from link-list bodies, including plain and Base64 lists and routing lines. Shared keys such as `announce` or `fragmentation-enable` are removed when either flag is set. `providerid`, `profile-title`, `subscription-userinfo`, and `subscription-refill-date` are kept.
- **Add:** create `happ.ini` and/or `incy.ini` next to `config.ini` (see [`happ.ini.example`](happ.ini.example) and [`incy.ini.example`](incy.ini.example)). Use the same section as in `config.ini` (for example, `[Sub_Main]`). The route's display `name` still works as an alias; if both are present, the matching `config.ini` section takes precedence. Section matching is case-insensitive for ASCII letters. Values are sent as response headers only to the matching client: a User-Agent product token of `Happ` or `INCY` (for example `Happ/3.23.0` or `INCY-Desktop/1.0`). A configured key also removes the same `#key:` line from the body, so the configured value wins. A blank value removes that parameter. Unknown keys for the file are ignored with a console warning; `providerid` is accepted only in `happ.ini`.
- **Provider ID:** the upstream `providerid` is always kept, from a header, a `#providerid` body line, or a `#?providerid=` subscription URL fragment. It is forwarded as a header, so it survives conversion to JSON/YAML targets. `providerid` in `happ.ini` replaces it for Happ clients; a blank value there is ignored rather than removing the upstream ID.

By default, SubBridge relays supported Happ and INCY subscription headers and link-list directives. It always blocks `new-url` and `new-domain` so an upstream cannot change the client's saved bridge URL. Stripping runs before local overrides. These rules apply to subscription response metadata, not settings inside node configurations or the [INCY Premium API](https://github.com/INCY-DEV/incy-docs/blob/main/ru/dev-docs/premium-api.en.md). Happ still validates the provider ID against the subscription domain.

For Happ requests, a route with exactly one HTTP(S) upstream sends its configured `link1` as `fallback-url`, even when stripping is enabled. A merged route never sends a fallback, even if only one upstream responds. SubBridge ignores upstream and INI `fallback-url` values. The original URL can contain a private subscription token, and User-Agent matching is not access control; keep these endpoints restricted to trusted clients.

With one upstream, Happ and INCY requests on the base port preserve the upstream format, including native Xray JSON. Explicit conversion ports still use their configured targets. Happ and INCY routes that merge multiple upstreams normalize to V2Ray links on the base port.

For example, for `[Sub_Main]` in `config.ini`:

```ini
# happ.ini
[Sub_Main]
subscriptions-sort-type = ping
profile-update-interval = 6
```

```ini
# incy.ini
[Sub_Main]
sort-order = ping
profile-update-interval = 6
```

Restart SubBridge after editing the files.

### Connecting from phones

SubBridge serves plain HTTP and has no authentication. Anyone who can reach a subscription port can read the subscription, including the original upstream URL sent to Happ as `fallback-url`. Restrict access to trusted devices and do not forward the ports on your router.

Use the PC's LAN address on the phone, for example `http://192.168.1.10:25501/`. `127.0.0.1` on a phone refers to the phone itself. If Happ times out, try the same URL on the same phone in a browser or another subscription client, with the phone on Wi-Fi and its VPN disconnected. If the other client reaches SubBridge but Happ does not, check Happ's local network access. On Android 17, go to **Settings → Apps → Happ → Permissions → Nearby devices** and tap **Allow**, then refresh the subscription.

In `--console` mode, each accepted connection logs a `[TCP]` line with the peer address before SubBridge reads the request. If there is no `[TCP]` entry, SubBridge did not accept a connection; check phone permissions, routing and the PC firewall. `[TCP]` without `[REQ]` on a subscription port means the connection opened but did not produce a complete HTTP GET; the console logs why, for example if the client sent TLS to the plain HTTP port.

INCY rejects cleartext HTTP, and SubBridge has no built-in HTTPS. A reverse proxy can provide HTTPS without making the subscription public, but TLS alone does not restrict who can read it. If you use a proxy, limit it to trusted devices on your LAN or private network using firewall or proxy access rules, use a certificate the client trusts, and block direct access to the backend from untrusted devices. Do not proxy the embedded subconverter on port `25500`.

---

## 🚀 Usage

### 1. Interactive / Console Mode
Run in the foreground with live logging:
```bash
./sub_bridge --console
```

### 2. Service Management Mode
Install, start, restart, or remove SubBridge as a background service:
```bash
# Windows (run as Administrator)
.\sub_bridge.exe --install
.\sub_bridge.exe --restart
.\sub_bridge.exe --uninstall

# Linux (run with sudo)
sudo ./sub_bridge --install
sudo ./sub_bridge --restart
sudo ./sub_bridge --uninstall
```
*Note: On Linux, SubBridge automatically registers a `systemd` service unit if `systemd` is present, or falls back to native daemonization otherwise.*

### 3. One-Shot File Converter
Convert any subscription URL or local file directly into a target configuration:
```bash
# Syntax: ./sub_bridge --convert <target> <url_or_file> [output_file] [--force-balancer]
# Targets: clash, singbox-android, singbox-pc, xray, xray-one, xray-jsons, v2ray

# Convert to Clash YAML
./sub_bridge --convert clash "https://example.com/sub" clash_config.yaml

# Convert to Sing-box JSON (Mobile/Android)
./sub_bridge --convert singbox-android "https://example.com/sub" singbox_android.json

# Convert to Sing-box JSON for Desktop / PC
./sub_bridge --convert singbox-pc "https://example.com/sub" singbox_pc.json

# Convert to native Xray JSONs Array ([...])
./sub_bridge --convert xray-jsons "https://example.com/sub" xray_configs.json

# Force generation of SubBridge Auto balancer group
./sub_bridge --convert clash "https://example.com/sub" clash_config.yaml --force-balancer
```

XHTTP VLESS/Trojan links preserve both `mode` and URL-encoded `extra`. In particular, `uplinkDataPlacement=header` requires `mode=packet-up`; dropping the mode can cause Xray to reject the resulting configuration.

#### Hysteria and Xray JSON output

Hysteria 2 nodes use Xray's native `"protocol": "hysteria"` and `"network": "hysteria"`, with `version: 2` in both the outbound settings and `hysteriaSettings`. They are not converted to VLESS. When the source is a native Xray Hysteria 2 outbound, its complete `streamSettings` is retained, including authentication, TLS options, and `finalmask.quicParams`. URI/normalized Hysteria 2 inputs get native settings with TLS and a default `h3` ALPN; Salamander obfuscation is emitted as a UDP finalmask.

This preserves the outbound, not the entire source profile: DNS, inbounds, and routing still follow the selected target's existing generation behavior. Xray-specific settings without share-link equivalents are preserved by JSON output, not guaranteed by link-only output.

Xray's native Hysteria implementation requires version 2. Attempting to synthesize a Hysteria 1 node in an Xray JSON target now fails explicitly (nonzero CLI exit or HTTP 422), rather than silently generating VLESS. Hysteria 1 link output remains available through `xray`/`v2ray`.

Unknown conversion targets are rejected: CLI conversion exits nonzero, HTTP conversion returns 400, and unsupported entries in `converts` are skipped with a warning. Skipped entries retain their port slot so later configured endpoints do not move.

### 4. Help & Version
```bash
./sub_bridge --help
./sub_bridge --version
```

### 5. Subconverter HTTP Endpoint
When SubBridge is running, connect your proxy client directly to the built-in subconverter port (`25500`):
```text
http://127.0.0.1:25500/sub?target=clash&url=http://127.0.0.1:25501
http://127.0.0.1:25500/sub?target=xray-jsons&url=https://example.com/sub
http://127.0.0.1:25500/sub?target=clash&force_balancer=true&url=https://example.com/sub
```

---

## 🧪 Testing

Run all unit and integration tests:

```bash
ctest --preset release --output-on-failure
```

Tests include:
- `ParserTest`: URI parsing, Base64 decoding, Xray JSON & gRPC node validation, balancer deduplication, Sing-box 1.14+ syntax, and `xray-jsons` generation
- `XrayTest`: Component generation, empty-input handling, XHTTP mode/extra round trips, and upstream balancer preservation when aggregating with unrelated gRPC nodes
- `XrayFidelityTest`: Offline CLI validation using an independent JSON parser and sanitized `leastLoad`, Hysteria 2, and gRPC Reality fixtures; not a client import test
- `SubscriptionParamsTest`: Happ and INCY INI overrides, plain/Base64 directive stripping, and provider ID preservation
- `HttpTargetTest` (POSIX): HTTP target validation and app-specific subscription responses using local fixtures, without a listening server
- `YamlTest`: Native Clash YAML node extraction, sanitization, and rule conversion
- `PythonYamlTest`: Reference YAML test verification

All tests use synthetic, sanitized dummy data without external dependencies.

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
