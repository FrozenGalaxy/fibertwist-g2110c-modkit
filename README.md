**Language:** English | [Deutsch](README.de.md)

# Genexis FiberTwist G2110C Modkit

Startup scripts, notes, and small helper tools for the **Genexis FiberTwist G2110C** ONT.

This repository uses a **serial-first** workflow. The SSH examples are only valid **after** the serial bootstrap has installed the persistent startup scripts and the ONT has rebooted.

1. Connect to the UART serial console.
2. Login with the observed manufacturer shell account.
3. Install the persistent `/var/config` startup scripts from serial.
4. Reboot the ONT.
5. After reboot, use SSH for normal access.
6. Optionally build/install the JSON diagnostic collector and cached HTTP server.

> This is not an official Genexis project. Use only on hardware you own or are explicitly allowed to administer. Do not clone GPON/OMCI identities or disrupt provider networks.

## Hardware notes

| Item | Value |
| --- | --- |
| Model | Genexis FiberTwist G2110C |
| SoC observed on this unit | Realtek RTL9602C |
| OS observed on this unit | Linux 3.18.24, Realtek Luna SDK 3.3.0 |
| Root filesystem | read-only squashfs |
| Persistent config | `/var/config` on YAFFS2 |
| Serial console | 3.3 V TTL UART, 115200 8-N-1 |
| Manufacturer serial login observed | `company` / `amyM77yY` |
| Default LAN/management IPs seen | `192.168.1.1/24`, `192.168.100.1/24` |

Observed transceiver identity:

```json
{
  "vendor_name": "T&W",
  "part_number": "",
  "serial_number": "TW2362G-CDEH",
  "module_id": "TW2362G-CDEH"
}
```

## Initial access: serial console

The first setup requires a hardware UART connection.

```text
3.3 V TTL UART
115200 baud
8-N-1
```

Observed serial login:

```text
company / amyM77yY
```

After the startup scripts are installed and the ONT is rebooted, serial should no longer be needed for normal work.

## Boot facts

BusyBox init runs `/etc/init.d/rcS`, and `rcS` executes `/etc/init.d/rc0` through `rc63`.

Observed relevant stages:

```text
rc2   mounts /var/config
rc3   starts configd, Realtek SDK, OMCI, BOSA, LAN stack
rc32  starts vendor startup/BOSA services
rc34  watchdog keepalive
rc35  runs late setup and calls /var/config/run_test.sh
```

`/etc/init.d` is read-only squashfs. `/var/config` is persistent YAFFS2. The reliable persistent hook is therefore:

```text
/var/config/run_test.sh
```

## Files installed on the ONT

```text
/var/config/ont_custom.conf       editable config
/var/config/run_test.sh           tiny rc35 entrypoint
/var/config/ont_startup.sh        main startup logic
/var/config/ont_http_start.sh     optional JSON HTTP server starter
/var/config/ont_startup.log       persistent log
```

## Serial bootstrap install

Run this from the ONT serial shell, not over SSH.

Create `/var/config/ont_custom.conf`:

```sh
cat > /var/config/ont_custom.conf <<'EOF'
ONT_MGMT_ENABLE=1
ONT_MGMT_IF=br0
ONT_MGMT_IP_CIDR=192.168.100.1/24
ONT_MGMT_WAIT_SECONDS=120
ONT_DELETE_BR0_TABLE252=1

# SSH/root user setup.
#
# ONT_USER_PASS_HASH must be a /var/passwd-compatible hash, not plaintext.
# Generate one on your Linux host, for example:
#
#   openssl passwd -1 'your-password-here'
#
# Or, if mkpasswd is available:
#
#   mkpasswd -m md5crypt 'your-password-here'
#
# Paste the full resulting hash below, including the leading "$1$...".
# The default placeholder intentionally disables creation of the configured root-equivalent SSH user.
ONT_USER_ENABLE=1
ONT_USER_NAME=root2
ONT_USER_PASS_HASH='YOUR_PASSWORD_HASH_HERE'
ONT_USER_UID=0
ONT_USER_GID=0
ONT_USER_GECOS=root2
ONT_USER_HOME=/tmp
ONT_USER_SHELL=/bin/sh

ONT_HTTP_ENABLE=0
ONT_HTTP_SCRIPT=/var/config/ont_http_start.sh
ONT_HTTPD_BIN=/var/config/ont_json_httpd
ONT_JSON_BIN=/var/config/diag_to_json
ONT_HTTP_BIND=192.168.100.1
ONT_HTTP_PORT=8090
ONT_HTTP_TTL=10
ONT_HTTP_TIMEOUT=8
ONT_HTTP_MAX_BYTES=524288
EOF
chmod 644 /var/config/ont_custom.conf
```

Before rebooting, replace `ONT_USER_PASS_HASH='YOUR_PASSWORD_HASH_HERE'` with a real password hash. If the placeholder is left unchanged, `ont_startup.sh` will refuse to create the configured root-equivalent user. In that case the ONT-side management IP may still be applied, but SSH as `root2` will not work and serial access will still be needed.

Install the repository scripts from serial. The easiest method is to paste the contents of these files into the matching path and then `chmod` them:

```text
scripts/run_test.sh        -> /var/config/run_test.sh        chmod 755
scripts/ont_startup.sh     -> /var/config/ont_startup.sh     chmod 755
scripts/ont_http_start.sh  -> /var/config/ont_http_start.sh  chmod 755
```

Then reboot from serial:

```sh
reboot
```

After reboot, the startup hook should:

- add `192.168.100.1/24` to `br0`
- remove `iif br0 lookup table 252`
- recreate the configured UID 0 user in `/var/passwd`
- optionally start the JSON HTTP server if enabled and installed

## After reboot: SSH workflow

Once the serial bootstrap has run, the ONT has rebooted, your router-side forwarding is configured, and `ONT_USER_PASS_HASH` was replaced with a real hash, SSH can be used. If the password hash was left as `YOUR_PASSWORD_HASH_HERE`, the startup script intentionally skips SSH user creation.

Example if your router forwards LAN port `2222` to ONT SSH:

```sh
ssh -p 2222 root2@192.168.1.1
```

Validate from SSH:

```sh
ip addr show br0; ip rule show; grep '^root2:' /var/passwd; tail -n 50 /var/config/ont_startup.log
```

## Building the C tools

The ONT is **MIPS big-endian**, uses **uClibc**, and expects this dynamic linker:

```text
/lib/ld-uClibc.so.0
```

You need a suitable MIPS big-endian uClibc cross compiler. One way is to build a small Buildroot toolchain. The compiler may be named something like:

```text
mips-buildroot-linux-uclibc-gcc
```

Set a variable to your compiler path:

```sh
export MIPS_CC=/path/to/mips-buildroot-linux-uclibc-gcc
```

Build `diag_to_json`:

```sh
$MIPS_CC -mips1 -EB -msoft-float -mno-mips16 -O2 -Wall -Wextra -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -o diag_to_json src/diag_to_json.c
```

Build `ont_json_httpd`:

```sh
$MIPS_CC -mips1 -EB -msoft-float -mno-mips16 -O2 -Wall -Wextra -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -o ont_json_httpd src/ont_json_httpd.c
```

Check the result:

```sh
file diag_to_json ont_json_httpd
```

Expected shape:

```text
ELF 32-bit MSB executable, MIPS
interpreter /lib/ld-uClibc.so.0
```

## Installing binaries after SSH works

Do this only after the serial bootstrap and reboot.

```sh
cat diag_to_json | ssh -p 2222 root2@192.168.1.1 'cat > /var/config/diag_to_json; chmod 755 /var/config/diag_to_json'
```

```sh
cat ont_json_httpd | ssh -p 2222 root2@192.168.1.1 'cat > /var/config/ont_json_httpd; chmod 755 /var/config/ont_json_httpd'
```

Enable HTTP in `/var/config/ont_custom.conf`:

```sh
sed -i 's/^ONT_HTTP_ENABLE=.*/ONT_HTTP_ENABLE=1/' /var/config/ont_custom.conf
```

Start it without rebooting:

```sh
/var/config/ont_http_start.sh
```

Test from a host that can reach the ONT management IP:

```sh
curl http://192.168.100.1:8090/info
```

## JSON tools

`diag_to_json` is a read-only collector. It does not execute vendor `diag`; it talks directly to Realtek raw socket / `getsockopt` backends and prints JSON to stdout.

`ont_json_httpd` is a tiny single-process HTTP wrapper around `diag_to_json`.

| Endpoint | Behavior |
| --- | --- |
| `/status` | return cached JSON; if stale, refresh after response |
| `/refresh` | refresh now and return JSON |
| `/info` | server/cache metadata |
| `/` | short help text |

## Router/OpenWrt reachability

The ONT-side startup only ensures the ONT has `192.168.100.1/24` on `br0`.

Your router still needs route/NAT/firewall setup if you want to reach the ONT from the LAN.

Typical setup:

```text
Router/OpenWrt side: 192.168.100.2/32 on the ONT-facing interface
ONT side:            192.168.100.1/24 on br0
```

Example exposed services:

```text
LAN :2222 -> ONT 192.168.100.1:22
LAN :2323 -> ONT 192.168.100.1:23
LAN :8088 -> ONT 192.168.100.1:80
LAN :8090 -> ONT 192.168.100.1:8090
```

## Repository layout

```text
.
├── LICENSE
├── README.md
├── docs/
│   └── ONT_CUSTOM_STARTUP.txt
├── scripts/
│   ├── ont_custom.conf
│   ├── ont_http_start.sh
│   ├── ont_startup.sh
│   └── run_test.sh
└── src/
    ├── diag_to_json.c
    └── ont_json_httpd.c
```

## License

This repository is licensed under **GNU AGPLv3-only** (`AGPL-3.0-only`) unless a file explicitly states otherwise.

AGPLv3 is a strong copyleft open-source license. In practice: if someone distributes modified versions, or runs a modified network-facing version for users, they must provide the corresponding source under the same license. This is intentionally stricter than permissive licenses like MIT/BSD/Apache.

The license covers only the original code, scripts, and documentation in this repository. It does not grant rights to vendor firmware, vendor binaries, Realtek SDK components, Genexis trademarks, ISP network identifiers, GPON/OMCI identities, or access to networks/devices you are not authorized to administer. See `LICENSE`.
