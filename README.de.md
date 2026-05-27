**Sprache:** [English](README.md) | Deutsch

# Genexis FiberTwist G2110C Modkit

Startup-Skripte, Notizen und kleine Hilfsprogramme für das **Genexis FiberTwist G2110C** ONT.

Dieses Repository verwendet bewusst einen **Serial-first**-Workflow. Die SSH-Beispiele funktionieren erst, **nachdem** der serielle Bootstrap die persistenten Startup-Skripte installiert hat und das ONT neu gestartet wurde.

1. Mit der UART-Seriellkonsole verbinden.
2. Mit dem beobachteten Hersteller-Shell-Account einloggen.
3. Die persistenten `/var/config`-Startup-Skripte über die serielle Konsole installieren.
4. Das ONT neu starten.
5. Nach dem Neustart SSH für den normalen Zugriff verwenden.
6. Optional den JSON-Diagnose-Collector und den gecachten HTTP-Server bauen/installieren.

> Dies ist kein offizielles Genexis-Projekt. Verwende es nur auf Hardware, die dir gehört oder die du ausdrücklich administrieren darfst. Klone keine GPON-/OMCI-Identitäten und störe keine Provider-Netze.

## Hardware-Hinweise

| Punkt | Wert |
| --- | --- |
| Modell | Genexis FiberTwist G2110C |
| Auf diesem Gerät beobachteter SoC | Realtek RTL9602C |
| Auf diesem Gerät beobachtetes OS | Linux 3.18.24, Realtek Luna SDK 3.3.0 |
| Root-Dateisystem | read-only squashfs |
| Persistente Konfiguration | `/var/config` auf YAFFS2 |
| Seriellkonsole | 3.3 V TTL UART, 115200 8-N-1 |
| Beobachteter Hersteller-Login über seriell | `company` / `amyM77yY` |
| Beobachtete Standard-LAN-/Management-IPs | `192.168.1.1/24`, `192.168.100.1/24` |

Beobachtete Transceiver-Identität:

```json
{
  "vendor_name": "T&W",
  "part_number": "",
  "serial_number": "TW2362G-CDEH",
  "module_id": "TW2362G-CDEH"
}
```

## Erstzugriff: Seriellkonsole

Die Ersteinrichtung benötigt eine Hardware-UART-Verbindung.

```text
3.3 V TTL UART
115200 Baud
8-N-1
```

Beobachteter serieller Login:

```text
company / amyM77yY
```

Nachdem die Startup-Skripte installiert wurden und das ONT neu gestartet wurde, sollte für die normale Nutzung kein serieller Zugriff mehr nötig sein.

## Boot-Fakten

BusyBox init startet `/etc/init.d/rcS`, und `rcS` führt `/etc/init.d/rc0` bis `rc63` aus.

Beobachtete relevante Phasen:

```text
rc2   mountet /var/config
rc3   startet configd, Realtek SDK, OMCI, BOSA und den LAN-Stack
rc32  startet weitere Vendor-Startup-/BOSA-Dienste
rc34  Watchdog-Keepalive
rc35  führt spätes Setup aus und ruft /var/config/run_test.sh auf
```

`/etc/init.d` liegt auf einem read-only squashfs. `/var/config` ist persistentes YAFFS2. Der zuverlässige persistente Hook ist daher:

```text
/var/config/run_test.sh
```

## Auf dem ONT installierte Dateien

```text
/var/config/ont_custom.conf       bearbeitbare Konfiguration
/var/config/run_test.sh           kleiner rc35-Einstiegspunkt
/var/config/ont_startup.sh        Hauptlogik für den Start
/var/config/ont_http_start.sh     optionaler Starter für den JSON-HTTP-Server
/var/config/ont_startup.log       persistentes Log
```

## Serial-Bootstrap installieren

Diese Befehle werden direkt in der seriellen Shell des ONT ausgeführt, nicht per SSH.

`/var/config/ont_custom.conf` erstellen:

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

Vor dem Neustart muss `ONT_USER_PASS_HASH='YOUR_PASSWORD_HASH_HERE'` durch einen echten Passwort-Hash ersetzt werden. Wenn der Platzhalter unverändert bleibt, erstellt `ont_startup.sh` den konfigurierten root-äquivalenten Benutzer absichtlich nicht. In diesem Fall kann die ONT-seitige Management-IP trotzdem gesetzt werden, aber SSH als `root2` funktioniert nicht und serieller Zugriff wird weiterhin benötigt.

Installiere die Repository-Skripte über die serielle Konsole. Die einfachste Methode ist, den Inhalt dieser Dateien in den passenden Zielpfad zu kopieren und danach `chmod` zu setzen:

```text
scripts/run_test.sh        -> /var/config/run_test.sh        chmod 755
scripts/ont_startup.sh     -> /var/config/ont_startup.sh     chmod 755
scripts/ont_http_start.sh  -> /var/config/ont_http_start.sh  chmod 755
```

Danach über die serielle Konsole neu starten:

```sh
reboot
```

Nach dem Neustart sollte der Startup-Hook:

- `192.168.100.1/24` zu `br0` hinzufügen
- `iif br0 lookup table 252` entfernen
- den konfigurierten UID-0-Benutzer in `/var/passwd` neu anlegen
- optional den JSON-HTTP-Server starten, sofern aktiviert und installiert

## Nach dem Neustart: SSH-Workflow

Sobald der serielle Bootstrap gelaufen ist, das ONT neu gestartet wurde, das routerseitige Forwarding eingerichtet ist und `ONT_USER_PASS_HASH` durch einen echten Hash ersetzt wurde, kann SSH verwendet werden. Wenn der Passwort-Hash weiterhin `YOUR_PASSWORD_HASH_HERE` ist, überspringt das Startup-Skript die Erstellung des SSH-Benutzers absichtlich.

Beispiel, wenn der Router LAN-Port `2222` auf ONT-SSH weiterleitet:

```sh
ssh -p 2222 root2@192.168.1.1
```

Validierung per SSH:

```sh
ip addr show br0; ip rule show; grep '^root2:' /var/passwd; tail -n 50 /var/config/ont_startup.log
```

## C-Tools bauen

Das ONT ist **MIPS big-endian**, verwendet **uClibc** und erwartet diesen dynamischen Linker:

```text
/lib/ld-uClibc.so.0
```

Du brauchst einen passenden MIPS-big-endian-uClibc-Cross-Compiler. Eine Möglichkeit ist ein kleines Buildroot-Toolchain-Setup. Der Compiler kann beispielsweise so heißen:

```text
mips-buildroot-linux-uclibc-gcc
```

Setze eine Variable auf den Pfad zu deinem Compiler:

```sh
export MIPS_CC=/path/to/mips-buildroot-linux-uclibc-gcc
```

`diag_to_json` bauen:

```sh
$MIPS_CC -mips1 -EB -msoft-float -mno-mips16 -O2 -Wall -Wextra -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -o diag_to_json src/diag_to_json_v5.c
```

`ont_json_httpd` bauen:

```sh
$MIPS_CC -mips1 -EB -msoft-float -mno-mips16 -O2 -Wall -Wextra -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -o ont_json_httpd src/ont_json_httpd.c
```

Ergebnis prüfen:

```sh
file diag_to_json ont_json_httpd
```

Erwartete Form:

```text
ELF 32-bit MSB executable, MIPS
interpreter /lib/ld-uClibc.so.0
```

## Binärdateien installieren, nachdem SSH funktioniert

Diesen Schritt erst nach Serial-Bootstrap und Neustart ausführen.

```sh
cat diag_to_json | ssh -p 2222 root2@192.168.1.1 'cat > /var/config/diag_to_json; chmod 755 /var/config/diag_to_json'
```

```sh
cat ont_json_httpd | ssh -p 2222 root2@192.168.1.1 'cat > /var/config/ont_json_httpd; chmod 755 /var/config/ont_json_httpd'
```

HTTP in `/var/config/ont_custom.conf` aktivieren:

```sh
sed -i 's/^ONT_HTTP_ENABLE=.*/ONT_HTTP_ENABLE=1/' /var/config/ont_custom.conf
```

Ohne Neustart starten:

```sh
/var/config/ont_http_start.sh
```

Von einem Host testen, der die Management-IP des ONT erreichen kann:

```sh
curl http://192.168.100.1:8090/info
```

## JSON-Tools

`diag_to_json` ist ein read-only Collector. Es führt nicht das Vendor-Tool `diag` aus, sondern spricht direkt mit Realtek-Raw-Socket- bzw. `getsockopt`-Backends und gibt JSON auf stdout aus.

`ont_json_httpd` ist ein kleiner Single-Process-HTTP-Wrapper um `diag_to_json`.

| Endpoint | Verhalten |
| --- | --- |
| `/status` | gibt gecachtes JSON zurück; wenn veraltet, wird nach der Antwort aktualisiert |
| `/refresh` | aktualisiert sofort und gibt JSON zurück |
| `/info` | Server-/Cache-Metadaten |
| `/` | kurze Hilfe |

## Router-/OpenWrt-Erreichbarkeit

Das ONT-seitige Startup stellt nur sicher, dass das ONT `192.168.100.1/24` auf `br0` hat.

Der Router benötigt weiterhin Routing-/NAT-/Firewall-Konfiguration, wenn das ONT aus dem LAN erreichbar sein soll.

Typisches Setup:

```text
Router/OpenWrt-Seite: 192.168.100.2/32 auf dem ONT-seitigen Interface
ONT-Seite:            192.168.100.1/24 auf br0
```

Beispiele für veröffentlichte Dienste:

```text
LAN :2222 -> ONT 192.168.100.1:22
LAN :2323 -> ONT 192.168.100.1:23
LAN :8088 -> ONT 192.168.100.1:80
LAN :8090 -> ONT 192.168.100.1:8090
```

## Repository-Struktur

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
    ├── diag_to_json_v5.c
    └── ont_json_httpd.c
```

## Lizenz

Dieses Repository ist unter **GNU AGPLv3-only** (`AGPL-3.0-only`) lizenziert, sofern eine Datei nicht ausdrücklich etwas anderes angibt.

AGPLv3 ist eine starke Copyleft-Open-Source-Lizenz. Praktisch bedeutet das: Wenn jemand veränderte Versionen verteilt oder eine veränderte netzwerkfähige Version für Nutzer betreibt, muss der zugehörige Quellcode unter derselben Lizenz bereitgestellt werden. Das ist bewusst strenger als permissive Lizenzen wie MIT/BSD/Apache.

Die Lizenz gilt nur für den ursprünglichen Code, die Skripte und die Dokumentation in diesem Repository. Sie gewährt keine Rechte an Vendor-Firmware, Vendor-Binaries, Realtek-SDK-Komponenten, Genexis-Marken, ISP-Netzwerkkennungen, GPON-/OMCI-Identitäten oder an Netzwerken/Geräten, die du nicht administrieren darfst. Siehe `LICENSE`.
