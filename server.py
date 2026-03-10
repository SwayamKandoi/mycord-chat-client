#!/usr/bin/env python3
import socket
import struct
import threading
import time
import datetime
import enum
import os
import signal

LOG_FILE = "messages.log"
LOG_ENTRIES = []
log_lock = threading.Lock()

clients = []   # list of (socket, username, ip)
clients_lock = threading.Lock()
running = True
server_socket = None  # Global reference to server socket for signal handlers


def is_ascii(s):
    """
    Python 3.6 compatible replacement for str.isascii() (added in Python 3.7)
    Checks if all characters in the string are ASCII (ord < 128)
    """
    return all(ord(c) < 128 for c in s)


class LogEntry:
    ip_address: str
    message_type: int
    timestamp: int
    username: str
    message: str

    def __init__(self, ip_address: str, message_type, username: str, message: str, timestamp: int = None):
        self.ip_address = ip_address
        self.message_type = message_type
        self.username = username
        self.message = message
        self.timestamp = timestamp or int(time.time())
    
    def serialize(self):
        return f"{self.ip_address}|{self.message_type}|{self.timestamp}|{self.username}|{self.message}"
    
    @staticmethod
    def deserialize(data):
        parts = data.split("|", 4)
        if len(parts) != 5:
            raise ValueError("Invalid data: " + str(data))
        ip_address = parts[0]
        message_type = int(parts[1])
        timestamp = int(parts[2])
        username = parts[3]
        message = parts[4]
        return LogEntry(ip_address, message_type, username, message, timestamp)

    def to_message(self):
        return Message(self.message_type, self.username, self.message, self.timestamp)
    
    @staticmethod
    def from_message(message: 'Message', ip_address: str = "0.0.0.0"):
        return LogEntry(ip_address, message.message_type, message.username, message.message, message.timestamp)


class Message:

    class MessageType(enum.Enum):
        MSG_LOGIN        = 0
        MSG_LOGOUT       = 1
        MSG_MESSAGE_SEND = 2
        MSG_MESSAGE_RECV = 10
        MSG_DISCONNECT   = 12
        MSG_SYSTEM       = 13

    USERNAME_LEN = 32
    MESSAGE_LEN = 1024
    MSG_FMT = "!II32s1024s"   # type, timestamp, username, message
    MSG_SIZE = struct.calcsize(MSG_FMT)

    message_type: int
    username: str
    message: str
    timestamp: int

    def __init__(self, message_type: int, username: str, message: str, timestamp: int = None):
        self.message_type = message_type
        self.username = username
        self.message = message
        self.timestamp = timestamp or int(time.time())
    
    def pack_message(self):
        uname_bytes = self.username.encode("utf-8")[:self.USERNAME_LEN-1]
        uname_bytes = uname_bytes + b"\x00" * (self.USERNAME_LEN - len(uname_bytes))  # Null pad
        msg_bytes = self.message.encode("utf-8")[:self.MESSAGE_LEN-1]
        msg_bytes = msg_bytes + b"\x00" * (self.MESSAGE_LEN - len(msg_bytes))  # Null pad
        return struct.pack(self.MSG_FMT, self.message_type, self.timestamp, uname_bytes, msg_bytes)

    @staticmethod
    def unpack_message(data):
        msg_type, ts, uname_bytes, msg_bytes = struct.unpack(Message.MSG_FMT, data)
        username = uname_bytes.split(b"\x00", 1)[0].decode("utf-8", errors="ignore")
        message = msg_bytes.split(b"\x00", 1)[0].decode("utf-8", errors="ignore")
        return Message(msg_type, username, message, ts)


def send_all(sock, data):
    """
    Helper to ensure no short writes
    """
    view = memoryview(data)
    while view:
        n = sock.send(view)
        if n == 0:
            raise ConnectionError("socket closed")
        view = view[n:]


def recv_all(sock, n):
    """
    Helper to ensure no short reads
    """
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf.extend(chunk)
    return bytes(buf)


def append_log(entry: LogEntry):
    """
    Append a log entry to the log entries list and write to the log file
    """
    with log_lock:
        LOG_ENTRIES.append(entry)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(entry.serialize() + "\n")
        f.flush()


def send_disconnect(sock, username, reason, ip):
    """
    Send to the client a disconnect message with the reason
    """
    print(f"[ERROR] {ip}: {username} {reason}")
    append_log(LogEntry(ip, Message.MessageType.MSG_DISCONNECT.value, username, reason))
    message = Message(Message.MessageType.MSG_DISCONNECT.value, username, reason)
    try:
        send_all(sock, message.pack_message())
    except Exception as e:
        print(f"[ERROR] send_disconnect(sock, {username}, {reason}, {ip}): {e}")


def broadcast_message(message_type: int, username: str, message: str):
    """
    Broadcast a message to all clients that are connected
    """
    try:
        message = Message(message_type, username, message)
        print(f"[MESSAGE] {message_type}\t{datetime.datetime.fromtimestamp(message.timestamp)}\t{username}: {message.message}")
        with clients_lock:
            for sock, u, ip in clients:
                try:
                    send_all(sock, message.pack_message())
                except Exception as e:
                    print(f"[ERROR] broadcast_message send_all({message_type}, {username}, {message}, {ip}): {e}")
    except Exception as e:
        print(f"[ERROR] broadcast_message({message_type}, {username}, {message}): {e}")


def client_thread(sock, addr):
    """
    Handle a client connection and all its recieved messages
    Three step procedure: 1) receive login, 2) send history messages, 3) continually listen for sends/logouts
    """
    ip = addr[0]
    username = ""
    message_times = []  # Track message times for rate limiting
    
    try:
        # 1) LOGIN
        # Can we receive the message?
        print(f"[INFO] Waiting for LOGIN from {ip}")
        try:
            # Set 5 second timeout for login message
            sock.settimeout(5.0)
            data = recv_all(sock, Message.MSG_SIZE)
            # Reset timeout to None (blocking) after successful login receive
            sock.settimeout(None)
        except Exception as e:
            print(f"[ERROR] client_thread receive {e}")
            send_disconnect(sock, "???", "Failed to receive LOGIN message within 5s", ip)
            return
        
        # Can we parse the message?
        try:
            msg = Message.unpack_message(data)
        except Exception as e:
            print(f"[ERROR] client_thread parse {e}")
            send_disconnect(sock, "???", "Failed to parse LOGIN message", ip)
            return
        
        # Is the message type LOGIN?
        if msg.message_type != Message.MessageType.MSG_LOGIN.value:
            send_disconnect(sock, "???", "First message must be LOGIN", ip)
            return
        
        # Validate username
        if not msg.username or not msg.username.strip():
            send_disconnect(sock, "???", "Username must not be empty", ip)
            return
        
        # Is the username ASCII and alphanumeric?
        if not is_ascii(msg.username) or not msg.username.isalnum():
            send_disconnect(sock, "???", "Username must be alphanumeric and ASCII", ip)
            return

        # Check if username is already connected
        with clients_lock:
            for _, u, _ in clients:
                if u == msg.username:
                    send_disconnect(sock, msg.username, "Username already connected", ip)
                    return
    
        # check if the username is in the do not assign list
        dn_list = ["SYSTEM", "SERVER", "ADMIN", "ROOT"]
        if msg.username in dn_list:
            send_disconnect(sock, msg.username, "Username is reserved", ip)
            return
        username = msg.username

        # 2) HISTORY
        print(f"[INFO] LOGIN succeeded for {username}({ip}). Sending history...")
        try:
            # Get the last 25 messages with MSG_MESSAGE_SEND type
            with log_lock:
                # Look for MESSAGE_SEND entries in the last 100 messages, chances are there are 25 message sends there
                history_messages = [
                    entry for entry in LOG_ENTRIES[-100:]
                    if entry.message_type == Message.MessageType.MSG_MESSAGE_SEND.value
                ][-25:]
            
            # Send each history entry as MSG_MESSAGE_RECV
            for entry in history_messages:
                print(entry.message)
                history_msg = Message(
                    Message.MessageType.MSG_MESSAGE_RECV.value,
                    entry.username,
                    entry.message,
                    entry.timestamp
                )
                send_all(sock, history_msg.pack_message())
        except Exception as e:
            print(f"[ERROR] client_thread history {e}")

        print(f"[INFO] History sent for {username}({ip}). Adding client to the broadcast list")
        # 3) join the clients list and broadcast the login
        with clients_lock:
            clients.append((sock, username, ip))
            num_connected = len(clients)
        append_log(LogEntry(ip, Message.MessageType.MSG_LOGIN.value, username, f"{username} logged in"))
        broadcast_message(Message.MessageType.MSG_SYSTEM.value, "SYSTEM", f"{username} logged in")
        
        # Send welcome message to the newly connected user
        welcome_msg = Message(Message.MessageType.MSG_SYSTEM.value, "SYSTEM", 
                             f"Welcome! There are {num_connected} user(s) connected. Type !help for commands.")
        try:
            send_all(sock, welcome_msg.pack_message())
        except Exception as e:
            print(f"[ERROR] Failed to send welcome message to {username}({ip}): {e}")

        # Set 30 minute timeout for message receiving
        TIMEOUT_SECONDS = 15 * 60  # 15 minutes
        sock.settimeout(TIMEOUT_SECONDS)

        # 4) Main loop
        while True:
            print(f"[INFO] Waiting for LOGOUT/MSGRECV from client")
            # Can we receive a message?
            try:
                data = recv_all(sock, Message.MSG_SIZE)
            except socket.timeout:
                print(f"[ERROR] client_thread timeout: Client {username}({ip}) did not send a message within {TIMEOUT_SECONDS} seconds")
                send_disconnect(sock, username, f"Disconnected due to timeout (no message received in {TIMEOUT_SECONDS // 60} minutes)", ip)
                break
            except Exception as e:
                print(f"[ERROR] client_thread receive message {e}")
                send_disconnect(sock, username, "Failed to receive message", ip)
                break
            
            # Can we parse the message?
            try:
                msg = Message.unpack_message(data)
            except Exception as e:
                print(f"[ERROR] client_thread parse message {e}")
                send_disconnect(sock, username, "Failed to parse message", ip)
                break
            
            # Is this a LOGOUT message?
            if msg.message_type == Message.MessageType.MSG_LOGOUT.value:
                print(f"[INFO] Client sent logout.")
                append_log(LogEntry(ip, Message.MessageType.MSG_LOGOUT.value, username, f"{username} logged out"))
                break
            
            # Is this a MESSAGE_SEND message?
            elif msg.message_type == Message.MessageType.MSG_MESSAGE_SEND.value:
                print(f"[INFO] Client sent a message")
                # Rate limiting: check if >5 messages in last second
                current_time = time.time()
                message_times = [t for t in message_times if current_time - t < 1.0]
                if len(message_times) >= 5:
                    print(f"[INFO] Client is spamming. Disconnecting client.")
                    send_disconnect(sock, username, "Too many messages at once (>5 in a second)", ip)
                    break
                message_times.append(current_time)
                
                # Check message validity
                if not msg.message:
                    send_disconnect(sock, username, "Messages must not be empty", ip)
                    break
                
                if "\n" in msg.message:
                    send_disconnect(sock, username, "Messages must not contain newlines", ip)
                    break
                
                if not is_ascii(msg.message) or not msg.message.isprintable():
                    send_disconnect(sock, username, "Messages must be ASCII", ip)
                    break
                
                # Log the message
                append_log(LogEntry(ip, Message.MessageType.MSG_MESSAGE_SEND.value, username, msg.message))

                # Check if the message is a command
                if msg.message == "!help":
                    send_all(sock, Message(Message.MessageType.MSG_SYSTEM.value, "SYSTEM", "Commands: !help, !list, !disconnect").pack_message())
                    continue
                elif msg.message == "!list":
                    with clients_lock:
                        user_list = [u for _, u, _ in clients]
                        user_list_str = ", ".join(user_list)
                        num_connected = len(clients)
                        message = f"There are {num_connected} user(s) connected: {user_list_str}"
                    send_all(sock, Message(Message.MessageType.MSG_SYSTEM.value, "SYSTEM", message).pack_message())
                    continue
                elif msg.message == "!disconnect":
                    send_disconnect(sock, username, "User asked to be disconnected", ip)
                    break
                
                # If it wasn't a command, broadcast the message to everyone
                print(f"[INFO] Broadcasting client message to everyone")
                broadcast_message(Message.MessageType.MSG_MESSAGE_RECV.value, username, msg.message)

            else:
                send_disconnect(sock, username, "Message type not supported", ip)
                break

    except ConnectionError:
        print(f"[INFO] Client {ip} disconnected")
    except Exception as e:
        print(f"[ERROR] client_thread({ip}): {e}")
        send_disconnect(sock, username, f"You caused a server error", ip)
    finally:
        with clients_lock:
            for i, (s, u, ip2) in enumerate(clients):
                if s is sock:
                    clients.pop(i)
                    break
        try:
            sock.close()
        except Exception as e:
            print(f"[ERROR] client_thread finally {e}")
        if username:
            broadcast_message(Message.MessageType.MSG_SYSTEM.value, "SYSTEM", f"{username} has disconnected")


def signal_handler(signum, frame):
    """
    Handle SIGINT and SIGTERM signals by closing the server socket.
    This causes srv.accept() to raise an exception, allowing the finally block to execute.
    """
    global server_socket, running
    signal_name = signal.Signals(signum).name
    print(f"[INFO] Received {signal_name}, shutting down...")
    running = False
    if server_socket:
        try:
            server_socket.close()
        except Exception as e:
            print(f"[ERROR] Error closing server socket in signal handler: {e}")


def main():
    import sys
    global running, server_socket

    # Register signal handlers for graceful shutdown
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # give the students a random port that is based on their username to avoid possible conflicts
    # they can specify with argv[1] an alternative port number if they want to
    port = hash(os.environ["USER"]) % (65535 - 2000) + 2000
    if len(sys.argv) >= 2:
        port = int(sys.argv[1])

    print("[INFO] Loading history")
    try:
        if not os.path.exists(LOG_FILE):
            print("[INFO] Creating new log file")
            amount = 30
            with open(LOG_FILE, "w"):
                random_usernames = ["abc123", "def456", "ghi789"]
                current_username = os.environ["USER"]
                for i in range(1, amount + 1):
                    append_log(
                        LogEntry(
                            "0.0.0.0", 
                            Message.MessageType.MSG_MESSAGE_SEND.value, 
                            random_usernames[i % len(random_usernames)], 
                            f"This is an old message {i}" + (f" with @{current_username} metion" if i % 28 == 0 else "")
                        )
                    )
        else:
            # Try to load existing log file
            amount = 0
            with open(LOG_FILE, "r") as f:
                for line in f.readlines():
                    line = line.strip()
                    if line:
                        try:
                            LOG_ENTRIES.append(LogEntry.deserialize(line))
                            amount += 1
                        except Exception as e:
                            print(f"[WARNING] Failed to load log entry: {e}")
                            continue
    except Exception as e:
            print(f"[ERROR] Failed to load history: {e}")
            return
    print(f"[INFO] Parsed {amount} messages from the history file")

    # start the server
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(300)
    server_socket = srv  # Store in global for signal handlers
    print(f"[INFO] mycord server listening on 0.0.0.0:{port}")

    try:
        while running:
            sock, addr = srv.accept()
            print(f"[INFO] Accepted connection from {addr[0]}:{addr[1]}")
            th = threading.Thread(target=client_thread, args=(sock, addr), daemon=True)
            th.start()
    except KeyboardInterrupt:
        print("[INFO] KeyboardInterrupt, shutting down...")
    except OSError:
        # Socket closed by signal handler or other means
        if running:
            raise  # Re-raise if it wasn't intentional
        print("[INFO] Server socket closed, shutting down...")
    finally:
        running = False
        print("[INFO] Closing server...")
        srv.close()
        print("[INFO] Sending disconnect messages and closing connections...")
        with clients_lock:
            for sock, u, ip in clients:
                try:
                    # Set 1 second timeout for disconnect send in case socket is dead
                    sock.settimeout(1.0)
                    send_disconnect(sock, u, "Server is shutting down", ip)
                except Exception as e:
                    print(f"[ERROR] Failed to send disconnect to {u}({ip}): {e}")
                finally:
                    # Always close the socket after attempting to send disconnect
                    try:
                        sock.close()
                    except Exception as e:
                        print(f"[ERROR] Failed to close socket for {u}({ip}): {e}")
            clients.clear()
        print("[INFO] Bye!")


if __name__ == "__main__":
    main()
