# P2P File Tracker

A distributed peer-to-peer file sharing system built from scratch in C. Multiple servers maintain a registry of available files and their locations. Clients register files, query the network for what's available, and download files directly from other clients — without the file data ever passing through the server.

Built across 7 incremental labs as part of a university networking course.

---

## How It Works

**Servers** join a UDP multicast group and maintain a linked list of every registered file — keyed by SHA-256 hash — along with the IP address and port of every client that holds it.

**Clients** have two roles simultaneously:

- **Holder:** Scans a local directory, hashes each file in 500KB chunks using SHA-256, and registers metadata with all servers via multicast. Listens for incoming chunk requests from other clients and serves them.
- **Requester:** Queries servers for available files, picks one, then contacts the holding client directly to download it chunk by chunk — validating each chunk's hash before writing it to disk, and validating the full file hash after reconstruction.

The server never touches file data. All transfers are client-to-client.

---

## Technical Details

**Language:** C  
**Networking:** UDP sockets, IP multicast, `select()` for non-blocking I/O  
**Cryptography:** OpenSSL EVP API — SHA-256 incremental hashing  
**Serialization:** cJSON — custom JSON protocol for all messages  
**Platform:** Linux (Ubuntu 24)

### Protocol

All messages are JSON over UDP. The chunk transfer itself uses a custom binary fragmentation protocol — each UDP packet carries an 8-byte header (fragment index + total fragments) followed by up to 1400 bytes of chunk data, keeping packets within Ethernet MTU to avoid IP-level fragmentation and packet loss.

| Message | Direction | Description |
|---|---|---|
| `upload` | client → servers (multicast) | Register a file with chunk hashes, size, and P2P port |
| `query` | client → servers (multicast) | Request list of available files |
| `queryResponse` | server → client (unicast) | File list with metadata, chunk hashes, and peer addresses |
| `getChunk` | client → client (unicast) | Request a specific chunk by its SHA-256 hash |
| raw binary | client → client (unicast) | Fragmented chunk data |

### Integrity Validation

Every chunk is validated with SHA-256 before being written to disk. After all chunks are received and assembled, the full file is hashed and compared to the hash stored in the server's registry. A mismatch at either level aborts the download.

### Chunked Hashing

Files are read once in 500KB passes. Each pass simultaneously hashes the chunk independently and updates the full-file hash context — so the full-file SHA-256 is computed in a single read with no re-reading.

---

## Getting Started

### Dependencies

```bash
sudo apt install libcjson-dev libssl-dev
```

### Build

```bash
make
```

### Run

Start one or more servers:

```bash
./server 224.0.0.1 1818
./server 224.0.0.1 1818
```

Start one or more clients, each pointing at a directory of files to share:

```bash
./client 224.0.0.1 1818 ./my_files
```

Each client will register all files in the given directory with every server on the multicast group, then present a menu:

```
Select an option:
1. View Available Files
2. Download a file
3. Exit
```

Option 1 queries all servers and displays every registered file with its name, size, and SHA-256 hash. Option 2 lets you select a file to download from a peer.

> **Note:** Uses UDP multicast. Must be run on a network that supports multicast traffic — a local machine or controlled network is recommended. Most public Wi-Fi and restricted campus networks block multicast.

---

## Engineering Challenges

**Packet loss from oversized UDP payloads.** An initial fragment payload of 60,000 bytes caused each UDP packet to exceed Ethernet MTU, triggering IP-level fragmentation. On the university network, roughly 33% of fragments were dropped. Reducing the payload to 1,400 bytes eliminated fragmentation and packet loss entirely.

**Network buffer overflow.** Sending hundreds of fragments in a tight loop overwhelmed the receiver's socket buffer even at 1,400 bytes. A 1ms delay between fragments gave the network time to clear without meaningfully affecting throughput.

**Stale fragment data corrupting future downloads.** After a failed download attempt, leftover fragments in the P2P socket buffer caused `select()` to return immediately on every subsequent iteration, spinning the menu loop without waiting for input. Fixed with a drain loop after each download attempt.

**Port registration.** Early versions stored the ephemeral source port captured by `recvfrom` as the peer's contact address. Downloaders would connect to a port nobody was listening on. Fixed by embedding the dedicated P2P port in the upload JSON payload so the server stores the correct port.

**Non-blocking I/O with dual roles.** The client needs to serve incoming chunk requests while also waiting for user input. Solved with `select()` watching both the P2P socket and `stdin` simultaneously — both events handled in the same iteration so neither blocks the other.

---

## File Structure

```
p2p-file-tracker/
├── client.c       — client logic: hashing, registration, querying, downloading, serving
├── server.c       — server logic: registry, query responses
├── Makefile
└── sample_files/  — sample files for testing
```

Each client creates a `CHUNKS/` subdirectory inside its file directory to store raw chunk data for serving. Add `CHUNKS/` to your `.gitignore`.

---

## Skills Demonstrated

- Systems programming in C — manual memory management, struct design, linked lists, heap allocation
- Network programming — UDP sockets, multicast, `sendto`/`recvfrom`, `SO_REUSEPORT`, `SO_RCVBUF`, `select()`
- Cryptography — SHA-256 via OpenSSL EVP, incremental hashing, binary-to-hex encoding
- Protocol design — custom JSON message types, binary fragmentation with reassembly
- Distributed systems — peer-to-peer transfer, metadata-driven retrieval, decentralized architecture
- Debugging — use-after-free, socket buffer overflow, race conditions in select() event handling
