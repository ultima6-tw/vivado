#!/usr/bin/env python3A
# -*- coding: utf-8 -*-
"""
Final, event-driven, dual-channel AWG client.
- Connects to a command channel (9100) and a notification channel (9101).
- Uses a background thread to listen for precise per-list status notifications.
- The main thread waits for an IDLE notification before preloading the next list.
"""

import socket
import struct
import time
import threading
import queue

# ---------- Connection settings ----------
HOST = "wavegenz7.local"
CONTROL_PORT = 9100
NOTIFY_PORT = 9101
CONNECT_TIMEOUT = 3.0

# --- Thread synchronization primitives ---
# A thread-safe queue to pass the ID of a newly freed list from the
# notification thread to the main thread.
free_list_queue = queue.Queue()

# --- Protocol and Frame Generation Helpers (unchanged) ---
def pack_word(cmd: int, ch: int, tone: int, data20: int) -> int: return ((cmd & 0xF) << 28) | ((ch & 1) << 27) | ((tone & 0x7) << 24) | (data20 & 0xFFFFF)
def make_index_word(ch: int, tone: int, idx20: int) -> int: return pack_word(0x1, ch, tone, idx20)
def make_gain_word(ch: int, tone: int, g20: int) -> int: return pack_word(0x2, ch, tone, g20)
def make_commit_word() -> int: return (0xF << 28)
def frame_A(): return [make_index_word(0,0,0x001), make_gain_word(0,0,0x1FFFF), make_commit_word()]
def frame_B(): return [make_index_word(0,0,0x020), make_gain_word(0,0,0x1FFFF), make_commit_word()]

# --- Socket and Protocol Operation Helpers (unchanged) ---
def connect_socket(ip: str, port: int, timeout=CONNECT_TIMEOUT) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect((ip, port))
    return s
def op_Z_reset(s): s.sendall(b'Z')
def op_B_begin(s, list_id: int, total_frames: int): s.sendall(b'B' + struct.pack(">BI", list_id, total_frames))
def op_P_push(s, list_id: int, words):
    payload = b''.join(struct.pack(">I", w) for w in words)
    s.sendall(b'P' + struct.pack(">BH", list_id, len(words)) + payload)
def op_E_end(s, list_id: int): s.sendall(b'E' + struct.pack(">B", list_id))

# --- [NEW] Helper function to build a large batch of 'P' commands in memory ---
def build_p_command_batch(list_id: int, nframes: int) -> bytes:
    """
    Builds a single, large bytes object containing a sequence of 'P' commands
    for all frames in a list. The content alternates between frame_A and frame_B.
    """
    all_packets = []
    for i in range(nframes):
        # Determine which frame to use for this iteration
        words = frame_A() if i % 2 == 0 else frame_B()
        
        # Pack the word payload (e.g., [idx, gain, commit])
        payload = b''.join(struct.pack(">I", w) for w in words)
        
        # Construct the complete 'P' command packet (P + header + payload)
        packet = b'P' + struct.pack(">BH", list_id, len(words)) + payload
        
        # Add the packet to our list
        all_packets.append(packet)
        
    # Join all the small packets into one giant bytes object and return it
    return b''.join(all_packets)

# --- [MODIFIED] Notification Listener Thread ---
def notification_listener(host_ip: str, port: int):
    s_notify = None
    try:
        s_notify = connect_socket(host_ip, port)
        s_notify.settimeout(None)
        print(f"[NOTIFY] Connected to notification server on port {port}")
        
        buffer = b""
        while True:
            data = s_notify.recv(1024)
            if not data:
                print("\n[NOTIFY] Server closed notification channel.")
                break
            
            buffer += data
            while b'\n' in buffer:
                message_bytes, buffer = buffer.split(b'\n', 1)
                message = message_bytes.decode().strip()
                if not message: continue

                print(f"\n[NOTIFY] Received: {message}")
                
                # Parse "LIST<id>:<STATE>" message
                if ":" in message:
                    parts = message.split(':')
                    if len(parts) == 2 and parts[0].startswith("LIST") and parts[1] == "IDLE":
                        try:
                            list_id = int(parts[0][4:])
                            free_list_queue.put(list_id)
                        except ValueError:
                            print(f"[NOTIFY] Could not parse list_id from '{parts[0]}'")

    except Exception as e:
        print(f"\n[NOTIFY] Error in listener: {e}")
    finally:
        if s_notify: s_notify.close()
        print("[NOTIFY] Listener thread exiting.")
        free_list_queue.put(None) # Sentinel value to unblock the main thread

# --- [MODIFIED] Main logic using batch transfers for high performance ---
def main():
    s_control = None
    notify_thread = None
    try:
        print(f"[CLIENT] Resolving {HOST}...")
        ip = socket.gethostbyname(HOST)

        print(f"[CLIENT] Connecting to control port {ip}:{CONTROL_PORT}...")
        s_control = connect_socket(ip, CONTROL_PORT)
        print("[CLIENT] Control channel connected.")

        notify_thread = threading.Thread(target=notification_listener, args=(ip, NOTIFY_PORT))
        notify_thread.daemon = True
        notify_thread.start()

        print("[CLIENT] Sending RESET...")
        op_Z_reset(s_control)
        nframes = 20

        # --- [MODIFIED] 1. Initial Priming using a single batch transfer ---
        for list_id_to_prime in [0, 1]:
            print(f"[*] Main thread waiting for an IDLE slot...")
            free_id = free_list_queue.get(timeout=5.0)
            if free_id is None: raise ConnectionError("Notify thread died")
            
            print(f"    -> Priming list {free_id} with a batch of {nframes} frames...")
            op_B_begin(s_control, free_id, nframes)
            
            # Build the entire batch in memory first
            batch_data = build_p_command_batch(free_id, nframes)
            print(f"    -> Batch created ({len(batch_data)} bytes). Sending now...")
            # Send the entire batch in a single network operation
            s_control.sendall(batch_data)
                
            op_E_end(s_control, free_id)
        
        # --- [MODIFIED] 2. Main event-driven loop using a single batch transfer ---
        print("\n[CLIENT] Entering event-driven loop... Press Ctrl+C to exit.")
        while True:
            print(f"[*] Main thread waiting for a list to become free...")
            free_list_id = free_list_queue.get()
            
            if free_list_id is None:
                print("[CLIENT] Notification channel closed. Exiting.")
                break

            print(f"    -> IDLE signal for list {free_list_id} received. Preloading with a batch...")
            op_B_begin(s_control, free_list_id, nframes)
            
            # Build and send the batch in the same way
            batch_data = build_p_command_batch(free_list_id, nframes)
            print(f"    -> Batch created ({len(batch_data)} bytes). Sending now...")
            s_control.sendall(batch_data)
            
            op_E_end(s_control, free_list_id)

            print(f"    -> Preloading list {free_list_id} complete.")

    except (KeyboardInterrupt, queue.Empty):
        print("\n[CLIENT] Operation stopped or timed out.")
    # ... (rest of exception handling and finally block is the same) ...
    finally:
        print("[CLIENT] Closing connection.")
        if s_control: s_control.close()
        if notify_thread and notify_thread.is_alive():
             free_list_queue.put(None)
             notify_thread.join(timeout=1.0)
        print("[CLIENT] Done.")

if __name__ == "__main__":
    main()
