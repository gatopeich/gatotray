# GatoTray Collector

A minimal zero-dependency C daemon that collects process statistics and exposes them via an abstract Unix domain socket.

## Overview

The collector daemon (`collector`) samples `/proc` every second to gather process statistics including:
- CPU usage percentage (computed from delta utime+stime vs system jiffies)
- VmRSS (Resident Set Size - memory usage)
- Command name

It builds Top-N snapshots and stores them in a memory-mapped ring buffer cache file, preserving recent history across restarts.

## Features

- **Zero dependencies**: Uses only standard C library and Linux syscalls
- **Abstract Unix socket**: No filesystem socket (Linux-specific feature)
- **Memory-mapped cache**: Ring buffer preserves recent Top-N history
- **ASCII line protocol**: Simple text-based communication (no JSON)
- **Auto-spawn capable**: Client can automatically start the daemon if needed

## Building

```bash
make
```

This builds both `collector` (the daemon) and `client` (test client).

## Usage

### Running the Collector

```bash
# Run in foreground
./collector

# Run as daemon (background)
./collector -d

# Custom socket name
./collector -s my_socket_name

# Custom cache file location
./collector -c /path/to/cache.file

# Environment variable for socket name
GATOTRAY_SOCKET_NAME=my_socket ./collector
```

### Using the Client

```bash
# Get current top processes
./client -c TOP

# Get full history (last 60 snapshots)
./client -c HISTORY

# Auto-spawn collector if not running
./client -a -c TOP

# Custom socket name
./client -s my_socket_name -c TOP
```

## Protocol

The collector accepts simple ASCII commands over the abstract Unix socket:

### Commands

- `TOP` - Returns the most recent Top-N snapshot
- `HISTORY` - Returns all snapshots in chronological order
- `QUIT` - Close the connection

### Response Format

Each snapshot response consists of:

```
TIMESTAMP <unix_timestamp>
ENTRIES <count>
<pid> <cpu_percent> <rss_kb> <command>
<pid> <cpu_percent> <rss_kb> <command>
...
END
```

Example:
```
TIMESTAMP 1762557910
ENTRIES 3
1234 5.25 102400 firefox
5678 2.50 51200 chrome
9012 1.00 20480 gnome-shell
END
```

## Cache File Format

The cache file uses a simple binary format:

**Header:**
- Magic: 0x47415443 ("GATC")
- Version: 1
- Slot size: sizeof(Snapshot)
- Number of slots: 60 (default)
- Write index: Current ring buffer position

**Data:**
- Fixed-size slots arranged as a ring buffer
- Each slot contains one Snapshot structure
- Older entries are overwritten as the ring wraps

## Integration

The collector is designed to be called by the gatotray UI or any other client. The client library functions in `client.c` can be integrated into other programs:

```c
#include "client.h"

// Connect to collector
int sock = collector_connect(NULL);  // Use default socket name

// Send command
collector_send_command(sock, "TOP");

// Disconnect
collector_disconnect(sock);
```

For auto-spawning the collector if not running:

```c
int sock = collector_auto_spawn(NULL);
```

## Configuration

- **Socket name**: Set via `-s` option or `GATOTRAY_SOCKET_NAME` environment variable (default: `gatotray_collector`)
- **Cache file**: Set via `-c` option (default: `/tmp/gatotray_top.cache`)
- **Top-N count**: Hardcoded to 10 (modify `TOP_N_PROCESSES` in source)
- **History slots**: Hardcoded to 60 (modify `RING_BUFFER_SLOTS` in source)
- **Sample interval**: 1 second (modify `SAMPLE_INTERVAL_SEC` in source)

## Installation

```bash
# Install to /usr/bin
sudo make install

# Then run as
gatotray-collector
gatotray-client -c TOP
```

## Notes

- The abstract Unix socket is a Linux-specific feature
- The daemon automatically creates the cache file if it doesn't exist
- The cache file persists across daemon restarts, preserving history
- Multiple clients can connect simultaneously (up to 10 by default)
- The daemon handles SIGINT and SIGTERM for graceful shutdown
