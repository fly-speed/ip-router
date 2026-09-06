#!/bin/sh

set -eu

INSTALL_ROOT=${DNS_GATE_HOME:-/opt/soft/dns-gate}
MASTER_ROOT=${ACL_MASTER_HOME:-/opt/soft/acl-master}
SERVICE_CONF="$INSTALL_ROOT/conf/dns-gate.cf"
MASTER_SERVICES="$MASTER_ROOT/conf/services.cf"
MASTER_CTL="$MASTER_ROOT/bin/master_ctl"

require_master() {
	if [ ! -x "$MASTER_CTL" ]; then
		echo "acl-master control program not found: $MASTER_CTL" >&2
		echo "Install acl-master under $MASTER_ROOT before installing dns-gate." >&2
		exit 1
	fi
	if [ ! -d "$MASTER_ROOT/conf" ]; then
		echo "acl-master configuration directory not found: $MASTER_ROOT/conf" >&2
		exit 1
	fi
}

register_service() {
	require_master
	touch "$MASTER_SERVICES"
	if ! grep -Fqx "$SERVICE_CONF" "$MASTER_SERVICES"; then
		printf '%s\n' "$SERVICE_CONF" >> "$MASTER_SERVICES"
		echo "Registered $SERVICE_CONF in $MASTER_SERVICES"
	fi
}

unregister_service() {
	if [ ! -f "$MASTER_SERVICES" ]; then
		return
	fi
	temporary="$MASTER_SERVICES.dns-gate.$$"
	grep -Fvx "$SERVICE_CONF" "$MASTER_SERVICES" > "$temporary" || true
	mv "$temporary" "$MASTER_SERVICES"
	echo "Unregistered $SERVICE_CONF from $MASTER_SERVICES"
}

start_service() {
	require_master
	"$MASTER_CTL" -f "$SERVICE_CONF" -a start
}

stop_service() {
	if [ -x "$MASTER_CTL" ] && [ -f "$SERVICE_CONF" ]; then
		"$MASTER_CTL" -f "$SERVICE_CONF" -a stop || true
	fi
}

restart_service() {
	require_master
	stop_service
	"$MASTER_CTL" -f "$SERVICE_CONF" -a start
}

case "${1:-}" in
	install)
		register_service
		restart_service
		;;
	register)
		register_service
		;;
	unregister)
		unregister_service
		;;
	start)
		start_service
		;;
	stop)
		stop_service
		;;
	restart)
		restart_service
		;;
	remove)
		stop_service
		unregister_service
		;;
	*)
		echo "Usage: $0 {install|register|unregister|start|stop|restart|remove}" >&2
		exit 2
		;;
esac
