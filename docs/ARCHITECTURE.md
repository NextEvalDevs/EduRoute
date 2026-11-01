# EduRoute System Architecture

EduRoute is an adaptive examination, real-time integrity monitoring, and zero-loss synchronization engine. It features a zero-STL C++ core engineered for execution under strict memory boundaries (< 50 microseconds per ingestion packet) and a low-bandwidth web interface tailored for 2G networks and low-power client devices.

---

## 1. High-Level Architecture Overview

```mermaid
flowchart TD
    subgraph ClientLayer["Client & Web Dashboard Layer"]
        StudentBrowser["Student Exam Portal<br/>(LocalStorage Disk Cache & Monotonic Timer)"]
        TeacherUI["Proctor Live Dashboard<br/>(Leaderboard, Collusion Graph, WAL & Compression Stats)"]
        FlaskServer["Flask Application<br/>[dashboard/app.py]"]
        PyBridge["C-ABI ctypes Bridge<br/>[dashboard/bridge.py]"]
    end

    subgraph CABILayer["C ABI Gateway Layer [include/eduroute_c_api.h]"]
        CApi["C ABI Engine Controller<br/>[src/eduroute_c_api.cpp]"]
        WireParser["Packed Wire Header & MAC16 Validator"]
    end

    subgraph Div1["Division 1: Sync Engine & Persistence"]
        WAL["Write-Ahead Log (WAL Ring)<br/>[src/sync_engine/wal_ring.cpp]"]
        Merkle["Merkle DAG Revision Tree<br/>[src/sync_engine/merkle_dag.cpp]"]
        Huffman["Huffman Delta Micro-Binary Compressor<br/>[src/sync_engine/huffman_delta.cpp]"]
        DiskStorage[("Persistent Disk Files<br/>.bin, .bin.wal, .bin.dag")]
    end

    subgraph Div2["Division 2: Anti-Cheat & Collusion Network"]
        Kmer["K-mer Winnowing Fingerprinter<br/>[src/anticheat/kmer_winnow.cpp]"]
        Anomaly["Anomaly Stats (Time-Sync & Rare Errors)<br/>[src/anticheat/anomaly_stats.cpp]"]
        Collusion["Collusion Graph & DSU Cluster Forest<br/>[src/anticheat/collusion_graph.cpp]"]
    end

    subgraph Div3["Division 3: Adaptive Ranking & Evaluation"]
        IRT["Item Response Theory (1PL Rasch) Selector<br/>[src/adaptive_rank/irt_selector.cpp]"]
        Fenwick["Fenwick Tree Percentile Index<br/>[src/adaptive_rank/fenwick_score.cpp]"]
        AVL["Order-Statistic AVL Tree (Top-K Leaderboard)<br/>[src/adaptive_rank/order_stat_avl.cpp]"]
    end

    subgraph CoreMemory["Core Zero-STL Infrastructure [include/core/]"]
        PersArena["Persistent Bump Arena<br/>(Exam Session & Global Indexes)"]
        ScratchArena["Scratch Bump Arena<br/>(Reset Every Ingest Packet)"]
        CoreDSU["Disjoint Set Union [core/dsu.hpp]"]
        CoreMap["Open-Addressing HashMap [core/hash_map.hpp]"]
        CoreArr["Dynamic Array [core/dyn_array.hpp]"]
    end

    StudentBrowser -->|"1. Submits answer or offline sync"| FlaskServer
    TeacherUI -->|"Queries live telemetry"| FlaskServer
    FlaskServer -->|"2. Dispatches wire request"| PyBridge
    PyBridge -->|"3. Invokes extern C ABI"| CApi
    CApi -->|"Validates frame"| WireParser

    WireParser -->|"Append delta snapshot"| Div1
    WAL -->|"Persist compressed deltas"| DiskStorage
    Merkle -->|"Append answer hash chain"| DiskStorage
    Div1 -->|"Huffman delta encoding"| Huffman

    WireParser -->|"Fingerprints & timestamps"| Div2
    Kmer -->|"Extracts minimum winnow hashes"| CoreMap
    Anomaly -->|"Tracks temporal/error coincidences"| CoreMap
    Collusion -->|"Merges clusters upon threshold W >= 3.0"| CoreDSU

    WireParser -->|"Option evaluation & scoring"| Div3
    IRT -->|"Calibrates theta ability via Newton-Raphson"| PersArena
    Fenwick -->|"Maintains score frequency distribution"| PersArena
    AVL -->|"Maintains exact rank ordering"| PersArena

    CApi -->|"Allocates persistent structures"| PersArena
    CApi -->|"Allocates per-packet temporary buffers"| ScratchArena
```

---

## 2. Ingestion Hot-Path Lifecycle

Every answer submission (MCQ or descriptive text revision) flows through a synchronous pipeline designed to execute in sub-millisecond time without heap allocations.

```mermaid
sequenceDiagram
    autonumber
    actor Student as Student Client
    participant Web as Flask & ctypes Bridge
    participant Engine as C ABI Engine
    participant Div1 as Div 1 (WAL & Merkle DAG)
    participant Div2 as Div 2 (Anti-Cheat & DSU)
    participant Div3 as Div 3 (IRT & AVL/Fenwick)
    participant Disk as On-Disk Storage

    Student->>Web: Submit Answer (or Replay Offline Disk Queue)
    Web->>Engine: eduroute_build_wire_packet() & eduroute_ingest_packet()
    
    activate Engine
    Engine->>Engine: Verify MAC-16 and FNV-1a DAG parent hash
    Engine->>Div1: Lookup previous answer for (student_id, question_id)
    Div1->>Div1: Generate delta & Huffman-compress payload
    Div1->>Disk: Append compressed record to .wal (CAT magic, checksum)
    Div1->>Disk: Append snapshot & FNV-1a node hash to .dag
    Div1->>Disk: Commit WAL sequence acknowledgment
    
    Engine->>Div2: Record monotonic elapsed time (Delta-t sync analysis)
    alt Action is TextDelta
        Engine->>Div2: Extract winnowed k-mer fingerprints & update containment
    else Action is MCQ Option
        Engine->>Div2: Grade choice & record rare-error shared mistakes
    end
    Div2->>Div2: Compute weight W = W_text + W_time + W_err
    opt Weight W >= 3.0
        Div2->>Div2: Merge student pair into DSU cheating ring cluster
    end

    Engine->>Div3: Submit option to active IRT ExamSession
    Div3->>Div3: Update theta ability via Newton-Raphson
    Div3->>Div3: Update Fenwick Tree cumulative score distribution
    Div3->>Div3: Update Order-Statistic AVL Tree (O(log N))
    Div3->>Div3: Select next optimal question via Fisher Information
    
    Engine->>Engine: Reset Scratch Arena (instant rollback)
    Engine-->>Web: Return 24-byte EduIngestResult struct
    deactivate Engine

    Web-->>Student: Display updated score, rank, percentile & next question
```

---

## 3. Division Architectural Breakdown

### Division 1: Synchronization & Persistence Subsystem

Division 1 guarantees zero data loss across network drops, sudden browser shutdowns, and power interruptions.

```mermaid
flowchart LR
    subgraph Div1Internal["Division 1 Components"]
        HD["Huffman Delta Engine<br/>(Binary micro-encoding)"]
        WALRing["Write-Ahead Log<br/>(Append-only disk buffer)"]
        DAGTree["Merkle DAG<br/>(Cryptographic answer history)"]
    end

    RawAnswers["Raw Answers<br/>(Text / MCQ)"] -->|"Compute difference"| HD
    HD -->|"CompressedTextDelta"| WALRing
    WALRing -->|"CAT record frame"| WALDisk[("Disk WAL File<br/>.bin.wal")]
    RawAnswers -->|"Answer snapshot"| DAGTree
    DAGTree -->|"FNV-1a hashed node"| DAGDisk[("Merkle DAG File<br/>.bin.dag")]
```

1. **Huffman Delta Micro-Binary Compressor (`huffman_delta.cpp`)**:
   - Generates minimal text differences between current draft and previous snapshot.
   - Computes dynamic 256-symbol frequency tables.
   - Encodes deltas into bitstream representations via custom `BitWriter`.
   - Typically achieves **50% to 85% bandwidth reduction** over raw ASCII deltas.
2. **Write-Ahead Log Ring (`wal_ring.cpp`)**:
   - Append-only file format with magic bytes `0x7f, 'C', 'A', 'T'` and file version 1.
   - Each record stores sequence, student ID, timestamp, Huffman frequency table (1024 bytes), and compressed bitstream payload.
   - Integrity verified using combined header-prefix and payload FNV-1a checksums.
   - Supports uncommitted states and sequence-based acknowledgments (`acknowledge(seq)`).
3. **Merkle DAG (`merkle_dag.cpp`)**:
   - Maintains an append-only cryptographic directed acyclic graph of student answer revisions.
   - Retains the 5 most recent snapshots per student-question pair in memory.
   - Nodes hashed deterministically via 64-bit FNV-1a combining `(student_id, question_id, parent_hash, timestamp, answer_bytes)`.
   - Replays on restart from the `.dag` disk file, restoring hash chains.

---

### Division 2: Real-Time Anti-Cheat & Collusion Subsystem

Division 2 detects real-time collusion networks between students using three independent detection dimensions:

```mermaid
flowchart TD
    subgraph Inputs["Telemetry Signals"]
        SignalText["Text Submissions"]
        SignalTime["Monotonic Time Deltas"]
        SignalChoice["MCQ Choices"]
    end

    subgraph Div2Engines["Anti-Cheat Processing"]
        KmerWinnow["K-mer Winnowing<br/>(k=5, w=4, Rabin-Karp)"]
        TimeAnomaly["Temporal Sync Detector<br/>(|dt_a - dt_b| <= 500ms)"]
        ErrorAnomaly["Rare Error Detector<br/>(Wrong choice frequency < 15%)"]
    end

    subgraph Aggregation["Graph & Clustering"]
        ColGraph["Collusion Graph<br/>W = W_text + W_time + W_err"]
        DSUForest["Disjoint Set Union (DSU)<br/>Cheating Ring Clusters"]
    end

    SignalText --> KmerWinnow
    SignalTime --> TimeAnomaly
    SignalChoice --> ErrorAnomaly

    KmerWinnow -->|"Text similarity containment"| ColGraph
    TimeAnomaly -->|"Temporal coincidence score"| ColGraph
    ErrorAnomaly -->|"Shared rare-error score"| ColGraph

    ColGraph -->|"Edge weight W >= 3.0"| DSUForest
```

1. **K-mer Winnowing Plagiarism Engine (`kmer_winnow.cpp`)**:
   - Uses sliding $k$-grams ($k=5$) with Rabin-Karp polynomial rolling hashing.
   - Selects minimum hashes across a sliding window of size $w=4$ to form a sparse fingerprint document vector.
   - Inverted hash index matches incoming fingerprints against all previous student submissions for that question.
   - Containment score computed as $|F_A \cap F_B| / \min(|F_A|, |F_B|)$.
2. **Statistical Anomaly Detector (`anomaly_stats.cpp`)**:
   - **Temporal Synchronization**: Identifies pairs of students submitting answers within a tight time window ($\le 500\text{ ms}$). Repeated synchronizations exponentially increment edge score.
   - **Shared Rare Errors**: Dynamically indexes wrong option selections. If two students pick the same distractor whose overall cohort selection frequency is $< 15\%$, it indicates collusion.
3. **Collusion Graph & DSU Cheating Clusters (`collusion_graph.cpp`)**:
   - Maintains edge state between student pairs $(A, B)$ with combined weight:
     $$W(A, B) = W_{\text{text}} + W_{\text{time}} + W_{\text{error}}$$
   - When $W(A, B) \ge 3.0$, the edge is confirmed as a collusion link.
   - The engine calls `dsu.unite(A, B)` to collapse colluding students into a unified disjoint cheating cluster.

---

### Division 3: Adaptive Ranking & Evaluation Subsystem

Division 3 handles question difficulty routing, scoring, and live leaderboards in logarithmic time.

```mermaid
flowchart LR
    subgraph Div3Engines["Adaptive Ranking Structures"]
        Bank["Question Bank<br/>(Item Response Theory)"]
        IRT["Fisher Information Router<br/>(1PL Rasch Model)"]
        Fenwick["Fenwick Tree<br/>(Score distribution [0..10000])"]
        AVL["Order-Statistic AVL<br/>(Score desc, Time asc, ID asc)"]
    end

    Bank --> IRT
    StudentAbility["Student Ability θ"] --> IRT
    IRT -->|"Next Question"| NextQ["Optimal Difficulty Question"]

    Submission["Graded Submission"] --> Fenwick
    Submission --> AVL

    Fenwick -->|"O(log M) query"| Percentile["Live Percentile (0 - 100%)"]
    AVL -->|"O(log N) query"| Rank["Exact 1-Based Leaderboard Rank"]
```

1. **Item Response Theory Selector (`irt_selector.cpp`)**:
   - Calibrated 1-Parameter Logistic (Rasch) IRT model where probability of correct response is:
     $$P(\theta) = \frac{1}{1 + e^{-a(\theta - \beta)}}$$
   - Questions selected to maximize Fisher Information $I(\theta) = a^2 P(\theta)(1 - P(\theta))$ at current ability estimate.
   - Student ability $\theta$ dynamically updated via Newton-Raphson iteration.
2. **Fenwick Score Tree (`fenwick_score.cpp`)**:
   - Binary Indexed Tree spanning scaled scores from 0 to 10,000.
   - Supports $O(\log M)$ point updates and prefix sum cumulative frequency queries.
   - Delivers instant percentile calculation:
     $$\text{Percentile} = 100.0 \times \frac{\text{prefix\_sum}(\text{score})}{\text{total\_students}}$$
3. **Order-Statistic AVL Tree (`order_stat_avl.cpp`)**:
   - Balanced binary search tree where each node maintains its subtree size.
   - Strict tie-breaking ordering:
     1. Higher scaled score ($\downarrow$)
     2. Lower monotonic time taken ($\uparrow$)
     3. Lower student ID ($\uparrow$)
   - Calculates exact 1-based leaderboard rank in $O(\log N)$ time with zero linear scans.

---

## 4. Memory Architecture & Two-Arena Lifecycle

EduRoute forbids standard library containers (`std::vector`, `std::string`, `std::map`) in the core engine, avoiding heap fragmentation and unpredictable latency spikes.

```mermaid
flowchart TD
    subgraph EngineMemory["EduRouteEngine Memory Layout"]
        subgraph PersArena["Persistent Arena (Lives for Entire Examination)"]
            P1["Question Bank & Item Calibrations"]
            P2["Student State Table array[max_students]"]
            P3["ExamSession Heap Allocations"]
            P4["Fenwick Array & AVL Tree Node Pool"]
            P5["Anti-Cheat Inverted Index & Collusion Graph"]
            P6["WAL In-Memory Record Ring Buffer"]
            P7["Merkle DAG History Snapshots"]
        end

        subgraph ScratchArena["Scratch Arena (Reset After Every Ingest Packet)"]
            S1["Unpacked Delta Buffers"]
            S2["Extracted K-mer Fingerprint Array"]
            S3["Text Similarity Candidate Array"]
            S4["Statistical Anomaly Match Array"]
            S5["Temporary Huffman Tree Frequency Map & Bit Buffer"]
            S6["Temporary Serialization Buffers"]
        end
    end

    Packet["Incoming Wire Frame"] -->|"1. Parse"| ScratchArena
    ScratchArena -->|"2. Process Hot-Path"| PersArena
    PersArena -->|"3. Commit State Updates"| PersArena
    PersArena -->|"4. Packet Done"| Reset["arena.reset() -> offset_ = 0"]
```

- **Persistent Arena**:
  - Pre-allocated once at engine creation (`eduroute_create`).
  - Sized to accommodate all students, questions, trees, and indexes plus slack space.
  - Zero dynamic heap re-allocations during active exam administration.
- **Scratch Arena**:
  - Sized at 2 MB per packet.
  - Used for temporary arrays, k-mer extractions, text serialization, and match candidate buffers.
  - At the end of every packet processing run, `scratch_arena.reset()` resets the bump pointer back to 0 in $O(1)$ time, guaranteeing zero memory leaks and cache locality.

---

## 5. Header Dependency Matrix (Static Inclusions)

The C++ core exhibits strict hierarchical layer separation:

```mermaid
graph TD
    classDef coreClass fill:#ccfbf1,stroke:#0f766e,stroke-width:1.5px;
    classDef div1Class fill:#e0e7ff,stroke:#4f46e5,stroke-width:1.5px;
    classDef div2Class fill:#ffe4e6,stroke:#e11d48,stroke-width:1.5px;
    classDef div3Class fill:#dcfce7,stroke:#16a34a,stroke-width:1.5px;
    classDef apiClass fill:#fef3c7,stroke:#d97706,stroke-width:1.5px;

    types["core/types.hpp"]:::coreClass
    arena["core/arena_alloc.hpp"]:::coreClass
    dynarray["core/dyn_array.hpp"]:::coreClass
    stringview["core/string_view.hpp"]:::coreClass
    hashmap["core/hash_map.hpp"]:::coreClass
    dsu["core/dsu.hpp"]:::coreClass
    bitstream["core/bit_stream.hpp"]:::coreClass

    types --> arena
    arena --> dynarray
    dynarray --> hashmap
    stringview --> hashmap
    arena --> hashmap
    dynarray --> dsu
    arena --> dsu
    dynarray --> bitstream

    huffman["sync_engine/huffman_delta.hpp"]:::div1Class
    wal["sync_engine/wal_ring.hpp"]:::div1Class
    merkle["sync_engine/merkle_dag.hpp"]:::div1Class

    arena --> huffman
    dynarray --> huffman
    stringview --> huffman
    huffman --> wal
    arena --> wal
    arena --> merkle
    dynarray --> merkle

    kmer["anticheat/kmer_winnow.hpp"]:::div2Class
    anomaly["anticheat/anomaly_stats.hpp"]:::div2Class
    collusion["anticheat/collusion_graph.hpp"]:::div2Class

    arena --> kmer
    hashmap --> kmer
    arena --> anomaly
    hashmap --> anomaly
    dsu --> collusion
    hashmap --> collusion

    bank["adaptive_rank/irt_selector.hpp"]:::div3Class
    fenwick["adaptive_rank/fenwick_score.hpp"]:::div3Class
    avl["adaptive_rank/order_stat_avl.hpp"]:::div3Class

    arena --> bank
    arena --> fenwick
    arena --> avl

    capi["eduroute_c_api.h"]:::apiClass
    capi_impl["src/eduroute_c_api.cpp"]:::apiClass

    capi --> capi_impl
    wal --> capi_impl
    merkle --> capi_impl
    huffman --> capi_impl
    kmer --> capi_impl
    anomaly --> capi_impl
    collusion --> capi_impl
    bank --> capi_impl
    fenwick --> capi_impl
    avl --> capi_impl
```
