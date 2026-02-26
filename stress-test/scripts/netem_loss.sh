#!/bin/bash
# netem_loss.sh — Simulate packet loss/delay on UDP port 4433 via tc/netem
set -e
PORT=4433
IFB=ifb0
IFACE=lo

start() {
    local loss="${1:-5}" delay="${2:-0}" jitter="${3:-0}" corr="${4:-0}"
    echo "Impairment: loss=${loss}% delay=${delay}ms jitter=${jitter}ms corr=${corr}%"
    stop_quiet
    modprobe ifb numifbs=1 2>/dev/null || true
    ip link set dev $IFB up 2>/dev/null || true
    tc qdisc add dev $IFACE handle ffff: ingress 2>/dev/null || true
    tc filter add dev $IFACE parent ffff: protocol ip u32 \
        match ip protocol 17 0xff match ip dport $PORT 0xffff \
        action mirred egress redirect dev $IFB
    local args="loss ${loss}%"
    [ "$corr" -gt 0 ] 2>/dev/null && args="loss ${loss}% ${corr}%"
    [ "$delay" -gt 0 ] 2>/dev/null && args="${args} delay ${delay}ms ${jitter}ms"
    tc qdisc add dev $IFB root netem $args
    echo "Active." && tc qdisc show dev $IFB
}
stop_quiet() {
    tc qdisc del dev $IFB root 2>/dev/null || true
    tc filter del dev $IFACE parent ffff: 2>/dev/null || true
    tc qdisc del dev $IFACE handle ffff: ingress 2>/dev/null || true
}
stop() { stop_quiet; echo "Impairment removed."; }
status() { echo "=== IFB ===" && tc qdisc show dev $IFB 2>/dev/null; echo "=== Filters ===" && tc filter show dev $IFACE parent ffff: 2>/dev/null; }
case "${1:-}" in
    start)  start "${2:-5}" "${3:-0}" "${4:-0}" "${5:-0}" ;;
    stop)   stop ;;
    status) status ;;
    *)      echo "Usage: $0 {start <loss%> [delay] [jitter] [corr]|stop|status}" ;;
esac
