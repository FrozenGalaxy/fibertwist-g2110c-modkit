#!/bin/sh

PATH=/bin:/sbin:/usr/bin:/usr/sbin
CONF=/var/config/ont_custom.conf
LOG=/var/config/ont_startup.log

[ -f "$CONF" ] && . "$CONF"

[ -n "$ONT_MGMT_ENABLE" ] || ONT_MGMT_ENABLE=1
[ -n "$ONT_MGMT_IF" ] || ONT_MGMT_IF=br0
[ -n "$ONT_MGMT_IP_CIDR" ] || ONT_MGMT_IP_CIDR=192.168.100.1/24
[ -n "$ONT_MGMT_WAIT_SECONDS" ] || ONT_MGMT_WAIT_SECONDS=120
[ -n "$ONT_DELETE_BR0_TABLE252" ] || ONT_DELETE_BR0_TABLE252=1
[ -n "$ONT_USER_ENABLE" ] || ONT_USER_ENABLE=1
[ -n "$ONT_USER_NAME" ] || ONT_USER_NAME=root2
[ -n "$ONT_USER_PASS_HASH" ] || ONT_USER_PASS_HASH='YOUR_PASSWORD_HASH_HERE'
[ -n "$ONT_USER_UID" ] || ONT_USER_UID=0
[ -n "$ONT_USER_GID" ] || ONT_USER_GID=0
[ -n "$ONT_USER_GECOS" ] || ONT_USER_GECOS="$ONT_USER_NAME"
[ -n "$ONT_USER_HOME" ] || ONT_USER_HOME=/tmp
[ -n "$ONT_USER_SHELL" ] || ONT_USER_SHELL=/bin/sh
[ -n "$ONT_HTTP_ENABLE" ] || ONT_HTTP_ENABLE=0
[ -n "$ONT_HTTP_SCRIPT" ] || ONT_HTTP_SCRIPT=/var/config/ont_http_start.sh

log_msg() { echo "$1" >> "$LOG"; }

valid_user_name() {
    case "$1" in ""|*:*|*/*|*\*|*" "*) return 1 ;; *) return 0 ;; esac
}

valid_pass_hash() {
    case "$1" in
        ""|"YOUR_PASSWORD_HASH_HERE"|"YOUR_PASSWORD"|*" "*)
            return 1
            ;;
        '$1$'*|'$5$'*|'$6$'*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

apply_user() {
    [ "$ONT_USER_ENABLE" = "1" ] || {
        log_msg "user creation disabled"
        return 0
    }

    if ! valid_user_name "$ONT_USER_NAME"; then
        log_msg "ERROR: invalid ONT_USER_NAME=$ONT_USER_NAME"
        return 1
    fi

    if ! valid_pass_hash "$ONT_USER_PASS_HASH"; then
        log_msg "ERROR: invalid or placeholder ONT_USER_PASS_HASH; user $ONT_USER_NAME not created"
        return 1
    fi

    if [ ! -f /var/passwd ]; then
        log_msg "ERROR: /var/passwd missing; user not applied"
        return 1
    fi

    LINE="$ONT_USER_NAME:$ONT_USER_PASS_HASH:$ONT_USER_UID:$ONT_USER_GID:$ONT_USER_GECOS:$ONT_USER_HOME:$ONT_USER_SHELL"
    TMP="/var/tmp/passwd.$$"

    grep -v "^$ONT_USER_NAME:" /var/passwd > "$TMP" 2>/dev/null
    echo "$LINE" >> "$TMP"

    if cat "$TMP" > /var/passwd 2>/dev/null; then
        rm -f "$TMP"
        log_msg "user $ONT_USER_NAME applied uid=$ONT_USER_UID gid=$ONT_USER_GID"
        return 0
    fi

    rm -f "$TMP"
    log_msg "ERROR: failed to update /var/passwd"
    return 1
}

apply_mgmt_once() {
    /bin/ip link show "$ONT_MGMT_IF" >/dev/null 2>&1 || return 1
    /bin/ip addr show dev "$ONT_MGMT_IF" 2>/dev/null | grep -q "inet $ONT_MGMT_IP_CIDR" || /bin/ip addr add "$ONT_MGMT_IP_CIDR" dev "$ONT_MGMT_IF" 2>/dev/null
    if [ "$ONT_DELETE_BR0_TABLE252" = "1" ]; then /bin/ip rule del iif "$ONT_MGMT_IF" table 252 2>/dev/null; fi
    log_msg "management applied iface=$ONT_MGMT_IF ip=$ONT_MGMT_IP_CIDR"
    return 0
}

apply_mgmt_wait() {
    i=0
    [ "$ONT_MGMT_ENABLE" = "1" ] || { log_msg "management IP disabled"; return 0; }
    while [ "$i" -lt "$ONT_MGMT_WAIT_SECONDS" ]; do
        if apply_mgmt_once; then log_msg "management ready attempt=$i"; return 0; fi
        i=$((i + 1)); /bin/sleep 1
    done
    log_msg "ERROR: management iface=$ONT_MGMT_IF not ready after ${ONT_MGMT_WAIT_SECONDS}s"; return 1
}

start_optional_http() {
    [ "$ONT_HTTP_ENABLE" = "1" ] || { log_msg "http server disabled"; return 0; }
    if [ ! -x "$ONT_HTTP_SCRIPT" ]; then log_msg "ERROR: ONT_HTTP_SCRIPT not executable: $ONT_HTTP_SCRIPT"; return 1; fi
    "$ONT_HTTP_SCRIPT"; return $?
}

log_msg "ont_startup begin"
apply_mgmt_wait; MGMT_RC=$?
apply_user; USER_RC=$?
start_optional_http; HTTP_RC=$?
log_msg "ont_startup done mgmt_rc=$MGMT_RC user_rc=$USER_RC http_rc=$HTTP_RC"
[ "$MGMT_RC" = "0" ] && [ "$USER_RC" = "0" ] && [ "$HTTP_RC" = "0" ] && exit 0
exit 1
