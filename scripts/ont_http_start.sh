#!/bin/sh

PATH=/bin:/sbin:/usr/bin:/usr/sbin
CONF=/var/config/ont_custom.conf
LOG=/var/config/ont_startup.log
PIDFILE=/var/run/ont_json_httpd.pid

[ -f "$CONF" ] && . "$CONF"
[ -n "$ONT_HTTP_ENABLE" ] || ONT_HTTP_ENABLE=0
[ -n "$ONT_HTTPD_BIN" ] || ONT_HTTPD_BIN=/var/config/ont_json_httpd
[ -n "$ONT_JSON_BIN" ] || ONT_JSON_BIN=/var/config/diag_to_json
[ -n "$ONT_HTTP_BIND" ] || ONT_HTTP_BIND=192.168.100.1
[ -n "$ONT_HTTP_PORT" ] || ONT_HTTP_PORT=8090
[ -n "$ONT_HTTP_TTL" ] || ONT_HTTP_TTL=10
[ -n "$ONT_HTTP_TIMEOUT" ] || ONT_HTTP_TIMEOUT=8
[ -n "$ONT_HTTP_MAX_BYTES" ] || ONT_HTTP_MAX_BYTES=524288

log_msg() { echo "$1" >> "$LOG"; }
[ "$ONT_HTTP_ENABLE" = "1" ] || { log_msg "ont_http_start: disabled"; exit 0; }
if [ ! -x "$ONT_HTTPD_BIN" ]; then log_msg "ont_http_start: missing/non-executable httpd: $ONT_HTTPD_BIN"; exit 1; fi
if [ ! -x "$ONT_JSON_BIN" ]; then log_msg "ont_http_start: missing/non-executable json collector: $ONT_JSON_BIN"; exit 1; fi
p=$(pidof ont_json_httpd 2>/dev/null)
if [ -n "$p" ]; then echo "$p" > "$PIDFILE" 2>/dev/null; log_msg "ont_http_start: already running pid=$p"; exit 0; fi
"$ONT_HTTPD_BIN" -a "$ONT_HTTP_BIND" -p "$ONT_HTTP_PORT" -t "$ONT_HTTP_TTL" -T "$ONT_HTTP_TIMEOUT" -m "$ONT_HTTP_MAX_BYTES" -c "$ONT_JSON_BIN" >> "$LOG" 2>&1 &
pid=$!
echo "$pid" > "$PIDFILE" 2>/dev/null
log_msg "ont_http_start: started pid=$pid bind=$ONT_HTTP_BIND port=$ONT_HTTP_PORT collector=$ONT_JSON_BIN"
exit 0
