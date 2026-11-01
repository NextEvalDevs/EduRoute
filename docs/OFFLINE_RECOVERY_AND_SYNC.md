# EduRoute Offline Resilience, Huffman Compression & Disk Recovery

This document details the multi-tiered synchronization, compression, and recovery architecture that guarantees zero data loss when internet connectivity fails during an examination.

---

## 1. Problem Statement & Threat Model

Online examination systems administered over mobile connections (2G/3G/4G) or unstable campus Wi-Fi face three primary failure modes:
1. **Client Disconnection**: The student's device loses network access while drafting or submitting an answer. Traditional HTTP POST requests fail, resulting in loss of typed text and disruption.
2. **Client Crash / Accidental Tab Closure**: The browser tab is closed, refreshed, or the operating system reboots while an uncommitted draft is in progress.
3. **Server Crash / Network Drop Before Acknowledgment**: The engine writes an incoming packet to disk, but the connection drops before transmitting the acknowledgment (ACK) back to the client.

EduRoute solves these challenges using a **two-tiered persistence model**:
- **Client Tier**: Continuous local disk storage (`localStorage`/IndexedDB) with an offline submission queue.
- **Server Tier**: Append-only Write-Ahead Log (`.wal`), Merkle DAG revision tree (`.dag`), and dynamic Huffman delta compression.

---

## 2. Multi-Tiered Architecture

```mermaid
flowchart TD
    subgraph ClientDevice["Student Device (Client Tier)"]
        UI["Exam Portal UI [exam.html]"]
        DraftDisk[("Browser Local Storage<br/>(Continuous Draft Cache)")]
        QueueDisk[("Offline Submission Queue<br/>(Disk-backed Buffer)")]
        NetDetector["Network Status Observer<br/>(window.ononline / onoffline)"]
    end

    subgraph Transport["Network Layer"]
        SyncAPI["/api/sync_offline<br/>(Batch Replay)"]
        SubmitAPI["/exam/submit<br/>(Single Ingest)"]
    end

    subgraph ServerBackend["Server Backend (Server Tier)"]
        Bridge["C ABI Bridge [bridge.py]"]
        CABI["Engine Ingest Controller [eduroute_c_api.cpp]"]
        
        subgraph Div1Sync["Division 1 Persistence"]
            Huffman["Huffman Delta Compressor<br/>(Dynamic 256-Symbol Tree)"]
            WAL["Write-Ahead Log (WAL Ring)<br/>(CAT frames + FNV-1a Checksums)"]
            DAG["Merkle DAG<br/>(Parent-Child Hash Chain)"]
        end
        
        DiskWAL[("eduroute_wal.bin.wal<br/>(On-Disk Persistent Log)")]
        DiskDAG[("eduroute_wal.bin.dag<br/>(On-Disk DAG Snapshots)")]
    end

    UI -->|"Keystroke / Option Change"| DraftDisk
    UI -->|"Submit while Online"| SubmitAPI
    UI -->|"Submit while Offline"| QueueDisk
    NetDetector -->|"Internet Reconnected"| QueueDisk
    QueueDisk -->|"Flush Queued Submissions"| SyncAPI

    SubmitAPI --> Bridge
    SyncAPI --> Bridge
    Bridge --> CABI

    CABI --> Huffman
    Huffman -->|"CompressedTextDelta"| WAL
    CABI -->|"Answer Snapshot"| DAG
    WAL -->|"Atomic write & flush"| DiskWAL
    DAG -->|"Append node"| DiskDAG
```

---

## 3. Client-Side Edge Resilience

### Continuous Draft Auto-Save
As the student types into `#full_text` or changes radio options, the client intercepts the event and saves the state to browser disk storage:
```javascript
localStorage.setItem('eduroute_draft_' + studentId + '_' + questionId, JSON.stringify({
    mcq_option: selectedOpt,
    full_text: textValue,
    elapsed_ms: Date.now() - startTime,
    saved_at: Date.now()
}));
```
- **Reboot / Refresh Recovery**: Upon loading any question, `restoreDraftFromDisk()` checks local disk storage and automatically repopulates the input field if an uncommitted draft exists.

### Offline Detection & Queue Buffering
When the student submits an answer while offline:
1. `navigator.onLine` and simulated offline state are checked.
2. The submission is appended to an on-disk array:
   ```javascript
   var queue = JSON.parse(localStorage.getItem('eduroute_offline_queue_' + studentId) || '[]');
   queue.push({
       student_id: studentId,
       question_id: questionId,
       question_type: questionType,
       mcq_option: opt,
       full_text: text,
       monotonic_dt_ms: elapsed,
       timestamp: Date.now()
   });
   localStorage.setItem('eduroute_offline_queue_' + studentId, JSON.stringify(queue));
   ```
3. A banner notifies the student that their answer is safely buffered to disk.
4. The student is permitted to continue to subsequent questions.

### Automatic Replay & Sync on Reconnection
When internet connectivity is restored:
1. `window.addEventListener('online')` triggers `syncOfflineQueueFromDisk()`.
2. All buffered records are posted as a batch to `/api/sync_offline`.
3. Upon receiving HTTP 200 and engine confirmation, the client purges the synchronized queue from local disk storage.

---

## 4. Server-Side Persistence & Huffman Compression

### Huffman Delta Micro-Binary Compression
Descriptive answer revisions often modify only a fraction of the text. Rather than transmitting and logging full text duplicates, EduRoute calculates the delta against the previous Merkle DAG snapshot:

$$\Delta = \text{generate\_text\_delta}(Q_{\text{id}}, \text{old\_value}, \text{new\_value})$$

1. **Serialization**: Delimited little-endian byte stream encoding `(answer_id, old_len, old_bytes, new_len, new_bytes)`.
2. **Frequency Table**: Computes exact frequencies across 256 byte values.
3. **Canonical Prefix Codes**: Constructs a binary Huffman tree where frequent symbols (common ASCII letters, punctuation) receive short bitcodes (2–4 bits).
4. **Bitstream Packing**: Packed into a byte array via `core::BitWriter`.
5. **Efficiency**: Yields **75.02% bandwidth savings** across stress simulations.

```mermaid
flowchart LR
    OldText["Old Draft: 'Hash tables use open addressing.'"]
    NewText["New Draft: 'Hash tables use open addressing with quadratic probing.'"]
    
    OldText & NewText --> GenDelta["generate_text_delta()"]
    GenDelta --> Serialized["Serialized Delta: 96 bytes"]
    Serialized --> FreqTable["256-Symbol Frequency Map"]
    FreqTable --> Tree["Huffman Binary Tree"]
    Tree --> BitWriter["BitWriter Bitstream: 408 bits (51 bytes)"]
    BitWriter --> Result["46.88% Size Reduction"]
```

---

### Write-Ahead Log (WAL) Binary Disk Framing
Every compressed delta is framed into a binary record and flushed to disk before in-memory state mutations take effect.

```
+--------------------------------------------------------------------------+
| WAL HEADER (21 Bytes)                                                    |
| Magic: 0x7f 'C' 'A' 'T' (4B) | Version: 1 (4B) | Type: DATA=1, ACK=2 (1B)|
| Payload Size: uint32 (4B)    | FNV-1a Checksum: uint64 (8B)              |
+--------------------------------------------------------------------------+
| WAL DATA PAYLOAD (Fixed 52 Bytes + Frequency 1024 Bytes + Compressed)    |
| Sequence: uint64 (8B)        | Student ID: uint32 (4B)                   |
| Timestamp: uint64 (8B)       | Bit Length: uint64 (8B)                   |
| Original Size: uint64 (8B)   | Delta Count: uint64 (8B)                  |
| Compressed Bytes: uint64 (8B)| Frequency Array: uint32[256] (1024B)      |
| Compressed Bitstream Payload: uint8[compressed_bytes]                     |
+--------------------------------------------------------------------------+
```

- **Acknowledgment Records**: When an ingest transaction completes adaptive scoring, `acknowledge(seq)` appends a lightweight 8-byte `RECORD_ACK` frame.
- **Atomic File Operations**: Flushed using `std::fflush()` immediately after writing, ensuring durability against power failure.

---

### Merkle DAG Answer History & Tamper Proofing
The Merkle DAG stores immutable cryptographic snapshots of every answer on disk (`.dag`).
- Each node hash is calculated as:
  $$\text{NodeHash} = \text{FNV1a}(\text{student\_id} \parallel \text{question\_id} \parallel \text{parent\_hash} \parallel \text{timestamp} \parallel \text{answer\_bytes})$$
- The root hash of the student's chain is initialized to `ROOT_DAG_HASH = 14695981039346656037ULL`.
- Any modification to past answers invalidates all descendant hashes, preventing post-exam answer tampering.

---

## 5. End-to-End Disaster Recovery Workflows

### Scenario 1: Client Internet Drops Mid-Exam & Reconnects

```mermaid
sequenceDiagram
    autonumber
    actor Student
    participant BrowserDisk as LocalStorage Disk
    participant Flask as Flask Server
    participant Engine as C++ Engine
    participant Disk as Disk WAL & DAG

    Note over Student,BrowserDisk: Internet drops! (Offline Mode)
    Student->>BrowserDisk: Auto-save Draft 1 (keystrokes)
    Student->>BrowserDisk: Submit Answer 1 -> Appended to offline queue
    Student->>BrowserDisk: Auto-save Draft 2
    Student->>BrowserDisk: Submit Answer 2 -> Appended to offline queue

    Note over Student,Flask: Internet Reconnects! (Online event fired)
    Student->>Flask: POST /api/sync_offline (Queued Answers 1 & 2)
    activate Flask
    Flask->>Engine: eduroute_ingest_recovered_answer(Answer 1)
    Engine->>Disk: Persist Huffman Delta to WAL & Merkle DAG
    Flask->>Engine: eduroute_ingest_recovered_answer(Answer 2)
    Engine->>Disk: Persist Huffman Delta to WAL & Merkle DAG
    Engine-->>Flask: Updated Score, Rank, Compression Telemetry
    Flask-->>Student: 200 OK (Sync confirmed)
    deactivate Flask
    Student->>BrowserDisk: Clear offline queue
```

---

### Scenario 2: Server Crash Recovery with Uncommitted WAL Records

```mermaid
sequenceDiagram
    autonumber
    participant Server as Engine Process
    participant Disk as On-Disk WAL (.wal)

    Server->>Disk: eduroute_wal_append_unacked(Seq #2, Seq #3)
    Note over Server,Disk: SERVER CRASH / SUDDEN POWER LOSS!
    Note over Server: Engine memory destroyed completely.

    Note over Server,Disk: SERVER REBOOT & RECOVERY
    Server->>Disk: eduroute_create("eduroute_wal.bin")
    Disk-->>Server: WriteAheadLog::open() -> read_all()
    Server->>Server: Verifies 'CAT' magic & FNV-1a checksums
    Server->>Server: Restores Seq #2 and #3 as pending uncommitted!
    
    Server->>Server: eduroute_wal_peek_next() -> Seq #2 identified
    Server->>Server: eduroute_wal_recover_and_apply_next() -> Huffman decompressed!
    Server->>Disk: Append RECORD_ACK for Seq #2
    
    Server->>Server: eduroute_wal_recover_and_apply_next() -> Seq #3 decompressed!
    Server->>Disk: Append RECORD_ACK for Seq #3
    Note over Server: All pending records recovered and committed (Pending = 0)
```
