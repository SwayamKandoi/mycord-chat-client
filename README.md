# MyCord Chat Client

A multithreaded TCP chat client written in **C** that communicates with a MyCord server using a custom binary messaging protocol.  
The client supports real-time messaging, mention highlighting, DNS-based connections, and graceful shutdown through signal handling.

This project demonstrates **low-level systems programming, networking, and concurrency in C on Linux.**

---

## Features

- TCP IPv4 client built with **POSIX socket programming**
- **Multithreaded architecture** using pthreads
- **Custom binary message protocol** for efficient communication
- **DNS resolution support** for domain-based connections
- Real-time message sending and receiving
- **@mention detection with terminal highlighting**
- Configurable connection settings via CLI flags
- Robust **input validation and error handling**
- Graceful termination via **SIGINT / SIGTERM**

---

## Architecture

The client uses a concurrent design:

**Main Thread**
- Reads user input from STDIN
- Validates messages
- Sends messages to the server

**Receive Thread**
- Continuously listens for messages from the server
- Parses incoming packets
- Formats and prints messages to the terminal

This allows **non-blocking chat interaction**, where messages can be received while the user is typing.

---

## Message Protocol

Communication with the server uses a **fixed-size binary packet (1064 bytes)**.

| Field | Size |
|------|------|
| Message Type | 4 bytes |
| Timestamp | 4 bytes |
| Username | 32 bytes |
| Message | 1024 bytes |

The protocol is implemented using packed C structs to ensure correct memory layout during transmission.

---

## Installation

Clone the repository:

```bash
git clone https://github.com/SwayamKandoi/mycord-chat-client.git
cd mycord-chat-client
```

Compile the client:

```bash
gcc client.c -o client -pthread
```

---

## Usage

Run the client:

```bash
./client
```

Connect to a specific port:

```bash
./client --port 1738
```

Connect using a domain:

```bash
./client --domain mycord.devic.dev
```

Show help menu:

```bash
./client --help
```

---

## Example Output

```
[2025-11-20 14:00:59] alice: Hello everyone
[2025-11-20 14:01:04] bob: Hi @alice
[SYSTEM] charlie joined the chat
```

Mentions of your username trigger a **terminal alert and red highlighting**.

---

## Technologies

- C
- POSIX Threads (pthreads)
- TCP/IP Socket Programming
- DNS Resolution
- Linux System Programming

---

## Key Concepts Demonstrated

- Concurrent network applications
- Custom binary protocol implementation
- CLI application development
- Thread-safe message handling
- Signal-based graceful shutdown

---

## License

MIT License
