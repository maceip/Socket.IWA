#!/usr/bin/env python3
"""quic_flood.py — Raw UDP stress test for QUIC echo server.
Sends configurable bursts of UDP packets to test server resilience.

Usage:
  python3 quic_flood.py --rate 10000 --duration 30
  python3 quic_flood.py --mode burst --packets 50000
  python3 quic_flood.py --mode ramp --max-rate 100000 --duration 60
  python3 quic_flood.py --mode chaos --duration 60
"""
import socket
import time
import argparse
import json
import random
import struct
import os

def make_quic_initial(dcid_len=8):
    """Construct a QUIC-like Initial packet (mostly valid header)."""
    flags = 0xc0  # Long header, Initial
    version = 0x00000001  # QUIC v1
    dcid = os.urandom(dcid_len)
    scid = os.urandom(8)
    token_len = 0
    payload = os.urandom(random.randint(100, 1100))

    pkt = struct.pack('>BI', flags, version)
    pkt += struct.pack('B', dcid_len) + dcid
    pkt += struct.pack('B', len(scid)) + scid
    pkt += struct.pack('B', token_len)
    pkt += struct.pack('>H', len(payload)) + payload
    return pkt

def make_garbage(size=None):
    """Random bytes."""
    return os.urandom(size or random.randint(20, 1200))

def make_short_header():
    """Short header with random DCID."""
    flags = 0x40  # Short header
    dcid = os.urandom(16)
    payload = os.urandom(random.randint(50, 800))
    return struct.pack('B', flags) + dcid + payload

PACKET_MAKERS = {
    'initial': make_quic_initial,
    'garbage': make_garbage,
    'short': make_short_header,
    'null': lambda: b'\x00' * random.randint(1, 100),
}

def run_constant(sock, addr, rate, duration, pkt_type):
    """Send at constant rate."""
    maker = PACKET_MAKERS.get(pkt_type, make_quic_initial)
    interval = 1.0 / rate if rate > 0 else 0
    sent = 0
    bytes_sent = 0
    start = time.monotonic()
    end = start + duration

    while time.monotonic() < end:
        pkt = maker()
        try:
            sock.sendto(pkt, addr)
            sent += 1
            bytes_sent += len(pkt)
        except Exception:
            pass
        if interval > 0:
            time.sleep(interval)

    elapsed = time.monotonic() - start
    return {'sent': sent, 'bytes': bytes_sent, 'elapsed': elapsed,
            'pps': sent / elapsed, 'mbps': bytes_sent * 8 / elapsed / 1e6}

def run_burst(sock, addr, packets, pkt_type):
    """Send N packets as fast as possible."""
    maker = PACKET_MAKERS.get(pkt_type, make_quic_initial)
    sent = 0
    bytes_sent = 0
    start = time.monotonic()

    for _ in range(packets):
        pkt = maker()
        try:
            sock.sendto(pkt, addr)
            sent += 1
            bytes_sent += len(pkt)
        except Exception:
            pass

    elapsed = time.monotonic() - start
    return {'sent': sent, 'bytes': bytes_sent, 'elapsed': elapsed,
            'pps': sent / elapsed if elapsed > 0 else 0,
            'mbps': bytes_sent * 8 / elapsed / 1e6 if elapsed > 0 else 0}

def run_ramp(sock, addr, max_rate, duration, pkt_type):
    """Ramp from 0 to max_rate over duration."""
    maker = PACKET_MAKERS.get(pkt_type, make_quic_initial)
    sent = 0
    bytes_sent = 0
    start = time.monotonic()
    end = start + duration

    while time.monotonic() < end:
        progress = (time.monotonic() - start) / duration
        current_rate = max(1, int(max_rate * progress))
        interval = 1.0 / current_rate

        pkt = maker()
        try:
            sock.sendto(pkt, addr)
            sent += 1
            bytes_sent += len(pkt)
        except Exception:
            pass
        time.sleep(interval)

    elapsed = time.monotonic() - start
    return {'sent': sent, 'bytes': bytes_sent, 'elapsed': elapsed,
            'pps': sent / elapsed, 'mbps': bytes_sent * 8 / elapsed / 1e6}

def run_chaos(sock, addr, duration, pkt_type):
    """Random mix of packet types, rates, and pauses."""
    sent = 0
    bytes_sent = 0
    start = time.monotonic()
    end = start + duration
    types = list(PACKET_MAKERS.values())

    while time.monotonic() < end:
        maker = random.choice(types)
        burst = random.randint(1, 100)
        for _ in range(burst):
            if time.monotonic() >= end:
                break
            pkt = maker()
            try:
                sock.sendto(pkt, addr)
                sent += 1
                bytes_sent += len(pkt)
            except Exception:
                pass
        # Random pause
        time.sleep(random.uniform(0, 0.1))

    elapsed = time.monotonic() - start
    return {'sent': sent, 'bytes': bytes_sent, 'elapsed': elapsed,
            'pps': sent / elapsed, 'mbps': bytes_sent * 8 / elapsed / 1e6}

def main():
    parser = argparse.ArgumentParser(description='UDP stress test for QUIC server')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=4433)
    parser.add_argument('--mode', default='constant', choices=['constant', 'burst', 'ramp', 'chaos'])
    parser.add_argument('--rate', type=int, default=10000, help='Packets/sec (constant mode)')
    parser.add_argument('--max-rate', type=int, default=100000, help='Max pps (ramp mode)')
    parser.add_argument('--duration', type=int, default=30, help='Duration in seconds')
    parser.add_argument('--packets', type=int, default=50000, help='Packet count (burst mode)')
    parser.add_argument('--packet-type', default='initial', choices=list(PACKET_MAKERS.keys()))
    parser.add_argument('--output', help='Save results JSON to file')
    args = parser.parse_args()

    addr = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f'Target: {args.host}:{args.port}')
    print(f'Mode: {args.mode} | Packet type: {args.packet_type}')

    if args.mode == 'constant':
        print(f'Rate: {args.rate} pps | Duration: {args.duration}s')
        result = run_constant(sock, addr, args.rate, args.duration, args.packet_type)
    elif args.mode == 'burst':
        print(f'Packets: {args.packets}')
        result = run_burst(sock, addr, args.packets, args.packet_type)
    elif args.mode == 'ramp':
        print(f'Max rate: {args.max_rate} pps | Duration: {args.duration}s')
        result = run_ramp(sock, addr, args.max_rate, args.duration, args.packet_type)
    elif args.mode == 'chaos':
        print(f'Duration: {args.duration}s')
        result = run_chaos(sock, addr, args.duration, args.packet_type)

    result['mode'] = args.mode
    result['packet_type'] = args.packet_type

    print(f'\nResults:')
    print(f'  Sent: {result["sent"]:,} packets')
    print(f'  Bytes: {result["bytes"]:,}')
    print(f'  Elapsed: {result["elapsed"]:.2f}s')
    print(f'  Rate: {result["pps"]:,.0f} pps')
    print(f'  Throughput: {result["mbps"]:.2f} Mbps')

    if args.output:
        with open(args.output, 'w') as f:
            json.dump(result, f, indent=2)
        print(f'  Saved: {args.output}')

    sock.close()

if __name__ == '__main__':
    main()
