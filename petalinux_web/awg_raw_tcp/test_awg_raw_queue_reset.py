#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Minimal AWG client for sending only the 'Z' (Reset) command.
- Connects to the control channel (9100).
- Sends a single 'Z' command.
- Disconnects.
"""

import socket
import struct
import time

# ---------- Connection settings ----------
HOST = "wavegenz7.local"  # Or use an IP address like "192.168.1.10"
CONTROL_PORT = 9100
CONNECT_TIMEOUT = 3.0

# --- Socket and Protocol Operation Helpers ---
def connect_socket(ip: str, port: int, timeout=CONNECT_TIMEOUT) -> socket.socket:
    """Establishes a TCP connection to the specified host and port."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1) # Disable Nagle's algorithm for lower latency
    print(f"[CLIENT] Attempting to connect to {ip}:{port}...")
    s.connect((ip, port))
    return s

def op_Z_reset(s):
    """Sends the 'Z' (Reset) command to the server."""
    print("[CLIENT] Sending 'Z' (Reset) command...")
    s.sendall(b'Z')
    print("[CLIENT] 'Z' command sent.")

# --- Main logic for Reset-only client ---
def main():
    s_control = None
    try:
        # Resolve hostname to IP address
        print(f"[CLIENT] Resolving {HOST}...")
        ip = socket.gethostbyname(HOST)
        print(f"[CLIENT] Resolved to IP: {ip}")

        # Connect to the control channel
        s_control = connect_socket(ip, CONTROL_PORT)
        print("[CLIENT] Control channel connected successfully.")

        # Send the Reset command
        op_Z_reset(s_control)

        print("[CLIENT] Reset operation complete.")

    except socket.timeout:
        print(f"[ERROR] Connection timed out after {CONNECT_TIMEOUT} seconds.")
    except socket.error as e:
        print(f"[ERROR] Socket error: {e}. Check server status and network connection.")
    except Exception as e:
        print(f"[ERROR] An unexpected error occurred: {e}")
    finally:
        # Ensure the socket is closed
        if s_control:
            print("[CLIENT] Closing control channel connection.")
            s_control.close()
        print("[CLIENT] Exiting.")

if __name__ == "__main__":
    main()