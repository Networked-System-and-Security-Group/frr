#!/usr/bin/env python3

import argparse
import ipaddress
import socket
import struct
import time


PM_MAGIC = 0x4D494452
PM_PORT = 5860


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def probe_payload(magic=PM_MAGIC, packet_type=0, seqno=1):
    return struct.pack("!IB3xIQ", magic, packet_type, seqno,
                       time.monotonic_ns() // 1000)


def ipv6_udp_packet(source, destination, payload, source_port=PM_PORT):
    source_bytes = ipaddress.IPv6Address(source).packed
    destination_bytes = ipaddress.IPv6Address(destination).packed
    udp_length = 8 + len(payload)
    pseudo_header = (source_bytes + destination_bytes
                     + struct.pack("!I3xB", udp_length, socket.IPPROTO_UDP))
    udp_header = struct.pack("!HHHH", source_port, PM_PORT, udp_length, 0)
    udp_checksum = checksum(pseudo_header + udp_header + payload)
    if udp_checksum == 0:
        udp_checksum = 0xFFFF
    udp_header = struct.pack("!HHHH", source_port, PM_PORT, udp_length,
                             udp_checksum)
    ipv6_header = struct.pack("!IHBB16s16s", 6 << 28, udp_length,
                              socket.IPPROTO_UDP, 64, source_bytes,
                              destination_bytes)
    return ipv6_header + udp_header + payload


def send_case(raw_socket, label, source, destination, payload,
              source_port=PM_PORT):
    packet = ipv6_udp_packet(source, destination, payload, source_port)
    try:
        raw_socket.sendto(packet, (destination, 0))
    except OSError as error:
        print(f"SKIP {label}: {error}", flush=True)
        return
    print(f"SENT {label}: {source} -> {destination}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--destination", required=True)
    parser.add_argument("--known-source", required=True)
    parser.add_argument("--unknown-source", default="fd00:dead::1")
    args = parser.parse_args()

    raw_socket = socket.socket(socket.AF_INET6, socket.SOCK_RAW,
                               socket.IPPROTO_RAW)
    cases = [
        ("unknown-source", args.unknown_source,
         probe_payload(seqno=0x10000001), PM_PORT),
        ("bad-magic", args.known_source,
         probe_payload(magic=0xDEADBEEF, seqno=0x10000002), PM_PORT),
        ("invalid-type", args.known_source,
         probe_payload(packet_type=9, seqno=0x10000003), PM_PORT),
        ("wrong-port", args.known_source,
         probe_payload(seqno=0x10000004), PM_PORT + 1),
        ("invalid-length", args.known_source, b"\0" * 8, PM_PORT),
        ("unexpected-reply", args.known_source,
         probe_payload(packet_type=1, seqno=0xFFFFFFFF), PM_PORT),
        ("mapped-source", "::ffff:192.0.2.1",
         probe_payload(seqno=0x10000005), PM_PORT),
        ("link-local-source", "fe80::bad",
         probe_payload(seqno=0x10000006), PM_PORT),
        ("multicast-source", "ff02::bad",
         probe_payload(seqno=0x10000007), PM_PORT),
    ]

    for label, source, payload, source_port in cases:
        send_case(raw_socket, label, source, args.destination, payload,
                  source_port)
        time.sleep(0.2)


if __name__ == "__main__":
    main()
