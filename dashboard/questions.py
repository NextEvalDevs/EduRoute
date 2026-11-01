"""
EduRoute Question Bank

Pre-calibrated question pool across core CSE subjects (DSA, OS, Networks).
Difficulty ratings (beta parameters) follow standard Item Response Theory (IRT)
calibration so the adaptive engine can tailor question difficulty to student ability.
"""

TOPICS = {
    0: "Data Structures",
    1: "Algorithms",
    2: "Operating Systems",
    3: "Computer Networks"
}

QUESTION_BANK = [
    # Topic 0: Data Structures (Difficulty Beta 700 - 1600)
    {
        "id": 1,
        "topic_id": 0,
        "difficulty_beta": 750.0,
        "expected_time_ms": 15000,
        "correct_mcq_option": 1,
        "type": "mcq",
        "prompt": "What is the worst-case time complexity of searching an element in a Balanced Binary Search Tree (AVL / Red-Black)?",
        "options": ["O(1)", "O(log n)", "O(n)", "O(n log n)"]
    },
    {
        "id": 2,
        "topic_id": 0,
        "difficulty_beta": 850.0,
        "expected_time_ms": 18000,
        "correct_mcq_option": 2,
        "type": "mcq",
        "prompt": "Which data structure is primarily used to implement Breadth-First Search (BFS)?",
        "options": ["Stack", "Priority Queue", "Queue (FIFO)", "Disjoint Set"]
    },
    {
        "id": 3,
        "topic_id": 0,
        "difficulty_beta": 1050.0,
        "expected_time_ms": 25000,
        "correct_mcq_option": 0,
        "type": "mcq",
        "prompt": "In a Fenwick Tree (Binary Indexed Tree), which bitwise operation isolates the lowest set bit of an integer x?",
        "options": ["x & (-x)", "x | (x + 1)", "x ^ (x >> 1)", "~x & (x - 1)"]
    },
    {
        "id": 4,
        "topic_id": 0,
        "difficulty_beta": 1250.0,
        "expected_time_ms": 30000,
        "correct_mcq_option": 3,
        "type": "mcq",
        "prompt": "What is the amortized time complexity per operation for Disjoint Set Union (DSU) with union-by-rank and path compression?",
        "options": ["O(log n)", "O(n)", "O(1)", "O(α(n)) where α is inverse Ackermann"]
    },
    {
        "id": 5,
        "topic_id": 0,
        "difficulty_beta": 1500.0,
        "expected_time_ms": 40000,
        "correct_mcq_option": 1,
        "type": "mcq",
        "prompt": "In an Order-Statistic AVL Tree, what additional metadata must each tree node maintain to support O(log n) rank queries?",
        "options": ["Parent pointer", "Subtree size (count of nodes)", "Depth from root", "Red/Black balance color"]
    },

    # Topic 1: Algorithms (Difficulty Beta 800 - 1700)
    {
        "id": 6,
        "topic_id": 1,
        "difficulty_beta": 800.0,
        "expected_time_ms": 20000,
        "correct_mcq_option": 0,
        "type": "mcq",
        "prompt": "Which algorithm finds the shortest path from a single source in a weighted graph with non-negative edge weights?",
        "options": ["Dijkstra's Algorithm", "Kruskal's Algorithm", "Floyd-Warshall Algorithm", "Kosaraju's Algorithm"]
    },
    {
        "id": 7,
        "topic_id": 1,
        "difficulty_beta": 1100.0,
        "expected_time_ms": 25000,
        "correct_mcq_option": 2,
        "type": "mcq",
        "prompt": "What is the average time complexity of QuickSelect to find the k-th smallest element in an unsorted array?",
        "options": ["O(n log n)", "O(log n)", "O(n)", "O(n^2)"]
    },
    {
        "id": 8,
        "topic_id": 1,
        "difficulty_beta": 1350.0,
        "expected_time_ms": 35000,
        "correct_mcq_option": 1,
        "type": "mcq",
        "prompt": "In Karp-Rabin string matching and text winnowing, how is the polynomial rolling hash efficiently updated when sliding the window by 1 character?",
        "options": [
            "Rehashing the whole window in O(k)",
            "Subtracting old character term and multiplying by base in O(1)",
            "Using bitwise XOR of ASCII codes",
            "Sorting the characters inside the window"
        ]
    },
    {
        "id": 9,
        "topic_id": 1,
        "difficulty_beta": 1600.0,
        "expected_time_ms": 45000,
        "correct_mcq_option": 3,
        "type": "mcq",
        "prompt": "Why does the Winnowing algorithm guarantee capturing at least one fingerprint from any identical substring of length >= (w + k - 1)?",
        "options": [
            "Because it computes SHA-256 over sentences",
            "Because it selects every prime hash",
            "Because it stores all possible substrings",
            "Because every window of size w selects its minimum hash position"
        ]
    },

    # Topic 2: Operating Systems (Difficulty Beta 750 - 1550)
    {
        "id": 10,
        "topic_id": 2,
        "difficulty_beta": 750.0,
        "expected_time_ms": 15000,
        "correct_mcq_option": 2,
        "type": "mcq",
        "prompt": "Which component of the OS kernel is responsible for saving and restoring CPU registers during a process context switch?",
        "options": ["Paging unit", "Virtual memory manager", "Scheduler & Dispatcher", "File system buffer cache"]
    },
    {
        "id": 11,
        "topic_id": 2,
        "difficulty_beta": 1150.0,
        "expected_time_ms": 25000,
        "correct_mcq_option": 0,
        "type": "mcq",
        "prompt": "Why is an Arena (bump) allocator significantly faster than standard malloc/free for short-lived request pipelines?",
        "options": [
            "Allocations are just pointer increments; zero free-list overhead, freed all at once",
            "It runs on the GPU using SIMD instructions",
            "It uses virtual memory swapping to compress bytes",
            "It automatically optimizes cache lines using JIT compilation"
        ]
    },
    {
        "id": 12,
        "topic_id": 2,
        "difficulty_beta": 1450.0,
        "expected_time_ms": 35000,
        "correct_mcq_option": 1,
        "type": "mcq",
        "prompt": "In Write-Ahead Logging (WAL), what fundamental database principle ensures durability before updating in-memory pages?",
        "options": [
            "Two-Phase Locking (2PL)",
            "WAL record must be flushed to disk before the corresponding page can be modified",
            "Eventual consistency via gossip protocols",
            "Hash-chain DAG signature validation"
        ]
    },

    # Topic 3: Computer Networks & Security (Difficulty Beta 850 - 1650)
    {
        "id": 13,
        "topic_id": 3,
        "difficulty_beta": 850.0,
        "expected_time_ms": 20000,
        "correct_mcq_option": 3,
        "type": "mcq",
        "prompt": "Which transport protocol guarantees in-order, reliable delivery of packets across an IP network?",
        "options": ["UDP", "ICMP", "ARP", "TCP"]
    },
    {
        "id": 14,
        "topic_id": 3,
        "difficulty_beta": 1200.0,
        "expected_time_ms": 30000,
        "correct_mcq_option": 0,
        "type": "mcq",
        "prompt": "In a Merkle DAG, how does a client prove to the server that its local state transition directly extends the previous state?",
        "options": [
            "Including the previous node's cryptographic hash as parent_hash in the submission",
            "Sending the entire history database on every request",
            "Using an RSA private key with timestamp",
            "Querying the DNS TXT record for the server head"
        ]
    },
    {
        "id": 15,
        "topic_id": 3,
        "difficulty_beta": 1550.0,
        "expected_time_ms": 40000,
        "correct_mcq_option": 2,
        "type": "mcq",
        "prompt": "How does FNV-1a hash algorithm combine each incoming byte with the 64-bit running hash accumulator?",
        "options": [
            "Add byte, then bitwise circular shift left by 13",
            "Multiply by FNV prime, then XOR the incoming byte",
            "XOR the incoming byte first, then multiply by the 64-bit FNV prime",
            "Compute CRC-32 and square modulo 2^64"
        ]
    },

    # Long-form / essay prompts for testing text diffing and k-mer winnowing
    {
        "id": 16,
        "topic_id": 0,
        "difficulty_beta": 1100.0,
        "expected_time_ms": 60000,
        "correct_mcq_option": 0,
        "type": "descriptive",
        "prompt": "Explain how a Disjoint Set Union (DSU) data structure detects connected components and how path compression optimizes find operations.",
        "options": []
    },
    {
        "id": 17,
        "topic_id": 1,
        "difficulty_beta": 1300.0,
        "expected_time_ms": 60000,
        "correct_mcq_option": 0,
        "type": "descriptive",
        "prompt": "Describe the Winnowing algorithm for document fingerprinting and how it detects localized plagiarism between two answers.",
        "options": []
    }
]

def get_question_by_id(qid: int) -> dict:
    for q in QUESTION_BANK:
        if q["id"] == qid:
            return q
    return None
