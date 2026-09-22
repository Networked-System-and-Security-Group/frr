#!/usr/bin/env python3

import argparse
import ipaddress
import socket
import struct
import time


CONTROL_PORT = 5859
CONTROL_VERSION = 4
REQUEST_LENGTH = 36


def encode_locator(address):
    locator = ipaddress.ip_address(address)
    if locator.version == 4:
        return struct.pack("!HH", 1, 0) + locator.packed + bytes(12)
    return struct.pack("!HH", 2, 0) + locator.packed


def encode_request(message_type, router_id, transport, asn, group_id):
    return (struct.pack("!BBH", CONTROL_VERSION, message_type, 0)
            + socket.inet_aton(router_id)
            + encode_locator(transport)
            + struct.pack("!II", asn, group_id))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--address-family", choices=("ipv4", "ipv6"),
                        required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--destination", required=True)
    args = parser.parse_args()

    family = socket.AF_INET if args.address_family == "ipv4" else socket.AF_INET6
    announce = encode_request(6, "10.255.1.2", args.source, 65102, 11)
    rep_list = encode_request(2, "10.255.1.2", args.source, 65102, 0)
    assert len(announce) == REQUEST_LENGTH

    udp_socket = socket.socket(family, socket.SOCK_DGRAM)
    udp_socket.bind((args.source, 0))
    udp_socket.sendto(announce, (args.destination, CONTROL_PORT))
    udp_socket.sendto(bytes([3]) + announce[1:],
                      (args.destination, CONTROL_PORT))
    udp_socket.sendto(bytes(8), (args.destination, CONTROL_PORT))
    udp_socket.close()

    tcp_socket = socket.socket(family, socket.SOCK_STREAM)
    tcp_socket.bind((args.source, 0))
    tcp_socket.settimeout(2)
    tcp_socket.connect((args.destination, CONTROL_PORT))
    tcp_socket.sendall(struct.pack("!I", len(rep_list)) + rep_list)
    time.sleep(0.2)
    tcp_socket.close()

    bad_tcp_socket = socket.socket(family, socket.SOCK_STREAM)
    bad_tcp_socket.bind((args.source, 0))
    bad_tcp_socket.settimeout(2)
    bad_tcp_socket.connect((args.destination, CONTROL_PORT))
    bad_tcp_socket.sendall(struct.pack("!I", 1))
    time.sleep(0.2)
    bad_tcp_socket.close()


if __name__ == "__main__":
    main()
