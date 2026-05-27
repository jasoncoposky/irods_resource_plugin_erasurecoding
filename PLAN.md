Phase 1: Core C++20 Plugin & Toolchain Integration
1.1. Plugin Scaffolding & Memory Safety

1.1.1. Initialize the iRODS resource plugin using C++20 features (e.g., std::span and std::string_view for zero-copy buffer handoffs).

1.1.2. Implement liberasurecode C-API wrappers to enforce strict RAII and prevent memory leaks during matrix generation.

1.1.3. Hook in dynamic backend parsing via the iRODS context string (enabling the runtime flip between hardware-agnostic Jerasure and SIMD-accelerated ISA-L).

1.2. libconveyor Pipeline Instantiation

1.2.1. Instantiate n dedicated libconveyor thread-safe dual ring-buffers within the plugin's start operation.

1.2.2. Tune the chunk boundaries to maximize throughput without triggering buffer saturation or memory thrashing on the coordinating node.

1.2.3. Wire up libconveyor's advanced observability hooks to expose queue depth and worker thread states for SRM monitoring.

Phase 2: The L3KVG Metadata Sidecar (The Control Plane)
2.1. Graph Schema Construction via lite3-cpp

2.1.1. Define the highly structured lite3-cpp JSON payloads for LOGICAL_FILE nodes (capturing logical_size, timestamps, and synthesized checksums).

2.1.2. Define the JSON schema for n FRAGMENT nodes and the STORED_ON relational edges to map the physical storage topography.

2.2. Zero-Parse Mutation Engineering

2.2.1. Expose the L3KVG ingest endpoint to the plugin, explicitly optimizing the commit pipeline to hit or exceed the ~9,767 ops/sec zero-parse mutation benchmark.

2.2.2. Implement idempotent UPSERT logic in the graph to absorb retry storms from transient network partitions.

2.3. The Tombstone & Reconciliation Daemon

2.3.1. Implement the PENDING_DELETE soft-delete state for unreachable physical nodes.

2.3.2. Deploy the asynchronous sweeper to periodically poll L3KV and execute cleanups against recovering remote resources.

Phase 3: The L1 Intercepts & iCAT Bypass
3.1. The redirect Hijack (Anti-Affinity Voting)

3.1.1. Intercept the L1 hierarchy resolution.

3.1.2. Execute internal child polling to guarantee a strict k+m Anti-Affinity dispersion layout.

3.1.3. Terminate the branch and return the single coordinating node vote to the iRODS agent, cleanly bypassing standard hierarchy nesting.

3.2. The fileStat Forgery

3.2.1. Intercept the L1 fileStat operation to block physical fragmentation sizing from bleeding into the iCAT.

3.2.2. Execute a rapid L3KVG lookup and synthesize a POSIX stat struct based exclusively on the lite3-cpp logical JSON payload.

Phase 4: The Shadow Fabric Write Path (Data Plane)
4.1. The Fragmentation Intercept

4.1.1. Intercept fileCreate / fileWrite and slice the incoming iRODS stream.

4.1.2. Dispatch the k data and m parity fragments into the n waiting libconveyor instances.

4.2. The Dual-Path L3 Dispatch

4.2.1. Evaluate locality: check host_resolution to determine local vs. remote targets.

4.2.2. Local Short-Circuit: Have libconveyor workers execute raw POSIX open()/write() directly to the physical vault_path.

4.2.3. Remote Dispatch: Have libconveyor workers drop the payload down to the native iRODS L3 API (rsFileOpen / rsFileWrite) for transparent RPC transport.

4.3. The Graph Lock

4.3.1. Synchronize libconveyor background worker completions.

4.3.2. Fire the final zero-parse mutation to lock the logical-to-physical mapping in L3KVG.

Phase 5: The Read Reassembly & Degraded Fallback
5.1. The Nominal Read (k-Subset Fetch)

5.1.1. Intercept fileOpen / fileRead and query L3KVG for the fragment topology.

5.1.2. Select the optimal k subsets based on node health/latency.

5.1.3. Trigger libconveyor or raw L3 reads to pull the chunks back into RAM, utilizing liberasurecode to stream the decoded plaintext back to the client.

5.2. On-the-Fly Matrix Inversion

5.2.1. Implement the timeout trigger: if a k fetch fails, dynamically substitute a surviving m parity fragment.

5.2.2. Pass the degraded matrix to liberasurecode for immediate CPU-level regeneration of the missing bytes without dropping the client stream.

Phase 6: The Unlink & Teardown Pipeline
6.1. The L1 Delete Intercept

6.1.1. Catch the fileUnlink operation before iRODS triggers the iCAT deletion.

6.1.2. Query L3KVG for the n physical vault paths.

6.2. Physical Scrubbing via libconveyor

6.2.1. Push the n physical paths into the background pipeline.

6.2.2. Execute rsFileUnlink or native POSIX unlink() to permanently scrub the shadow fabric.

6.3. Graph Cleanup & Acknowledgment

6.3.1. Drop successful nodes from L3KVG and tombstone any unreachable ones.

6.3.2. Return the final success code to the L1 agent.
