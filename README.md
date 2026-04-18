# p2p-file-tracker

A client-server file registry built in C using UDP multicast. Clients scan a directory, hash each file in 500KB chunks using SHA-256, and send file metadata as JSON to the server. The server maintains a linked list of unique files keyed by full-file hash and tracks every peer that possesses each file. Duplicate registrations from the same client are ignored.

## Dependencies
- `libcjson`
- OpenSSL (`libssl`, `libcrypto`)

```
sudo apt install libcjson-dev libssl-dev
```

## Build
```
make
```

## Usage
Start the server:
```
./server 224.0.0.1 5000
```
Run the client:
```
./client 224.0.0.1 5000 ./your_directory
```

> **Note:** This project uses UDP multicast. Multicast must be supported and allowed on your network for the client and server to communicate. It will not work on networks that block multicast traffic (e.g. most public Wi-Fi or restricted campus networks). Testing on a local machine or a network you control is recommended.

## JSON Packet Format
Each file in the directory produces one JSON packet sent from client to server:
```json
{
  "filename": "example.txt",
  "fileSize": 1048576,
  "numberOfChunks": 2,
  "chunk_hashes": ["a1b2c3...", "d4e5f6..."],
  "fullFileHash": "deadbeef..."
}
```
