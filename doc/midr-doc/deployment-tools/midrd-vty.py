#!/usr/bin/env python3
import argparse
import socket
import sys

class VTY:
    def __init__(self, path, timeout):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)

    def execute(self, command):
        if "\0" in command or "\n" in command or "\r" in command:
            raise ValueError("one command per line")
        self.sock.sendall(command.encode() + b"\0")
        data = bytearray()
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("midrd VTY socket closed")
            data.extend(chunk)
            end = data.find(b"\0\0\0")
            if end >= 0 and len(data) >= end + 4:
                return data[:end].decode(errors="replace"), data[end + 3]

    def close(self):
        self.sock.close()

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--socket", default="/run/midr/midrd.vty")
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("-c", "--command", action="append")
    p.add_argument("--commands-file")
    args = p.parse_args()

    commands = args.command
    if args.commands_file:
        with open(args.commands_file, encoding="utf-8") as f:
            commands = [
                line.strip() for line in f
                if line.strip() and not line.lstrip().startswith(("!", "#"))
            ]

    client = VTY(args.socket, args.timeout)
    try:
        output, status = client.execute("enable")
        if status:
            raise RuntimeError(output)

        if commands is not None:
            for command in commands:
                output, status = client.execute(command)
                print(output, end="", flush=True)
                if status:
                    raise RuntimeError(f"command failed: {command}")
            return

        while True:
            try:
                command = input("midrd> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return
            if command in ("exit", "quit"):
                return
            if command:
                output, status = client.execute(command)
                print(output, end="", flush=True)
    finally:
        client.close()

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as e:
        print(f"midrd-vty: {e}", file=sys.stderr)
        sys.exit(1)
