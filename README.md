# iRODS Erasure Coding Resource Plugin

A high-performance, resilient, and database-agnostic erasure coding resource plugin for iRODS 5.x. This plugin implements a "Parallel Dispersive Storage" architecture that decouples data movement from metadata management to achieve extreme throughput and fault tolerance.

## 1. High-Level Architecture

The plugin utilizes a unique **Two-Plane Architecture**:

### Data Plane: High-Concurrency Movement
*   **Engine**: Built on **libconveyor**, a production-grade asynchronous buffering library.
*   **Parallel Dispatch**: Every data and parity fragment is handled by an independent, non-blocking I/O pipeline.
*   **Zero-Copy Path**: Employs C++20 `std::span` and pre-allocated 32MB buffers to minimize memory moves, enabling saturation of local memory-bus speeds during ingest.
*   **Encoding**: Uses **liberasurecode** (Reed-Solomon) for mathematical fragment generation.

### Control Plane: Shadow Metadata Graph
*   **Engine**: Integrated with **L3KVG** (via `lite3-cpp`), a high-performance graph metadata sidecar.
*   **Bypass Strategy**: Fragment mappings are stored in a distributed graph, completely bypassing the iCAT relational database for fragment discovery and recovery.
*   **Anti-Affinity**: Intelligent redirection logic analyzes the physical topology to ensure fragments are dispersed across distinct physical hosts, preventing data loss during node failure.

## 2. Key Features

- **Distributed Read Recovery**: Transparently reconstructs missing or corrupted fragments on-the-fly. If a storage node is down, parity blocks are automatically fetched and decoded without user intervention.
- **Differentiated Error Handling**: Uses a centralized error table (`irods_erasure_coding_error_codes.hpp`) for precise diagnostics of I/O, pipeline, and control plane failures.
- **Optimal Dispersion**: Mathematical anti-affinity voting ensures k+m fragments are never co-located on the same physical hardware.

## 3. Configuration & Deployment

### 3.1. Prerequisites
- iRODS 5.0.0+
- `liberasurecode-dev`
- `libconveyor`
- `L3KVG` (running as a sidecar)

### 3.2. Registering the Resource
The Erasure Coding resource is a **Coordinating Resource**. It must be placed in a hierarchy above at least `k + m` storage resources.

```bash
# Correct syntax: name, type, host:path, context
iadmin mkresc ec_resc erasurecoding <hostname>:"" "k=4;m=2;backend=jerasure"

# Link child storage resources
iadmin addchildtoresc ec_resc leaf_resc_1
iadmin addchildtoresc ec_resc leaf_resc_2
...
iadmin addchildtoresc ec_resc leaf_resc_6
```

### 3.3. Tuning Parameters
Tune the engine via the resource context string:
```bash
iadmin modresc ec_resc context "k=4;m=2;backend=jerasure;chunk_size=33554432"
```

- `k`: Number of data fragments.
- `m`: Number of parity fragments.
- `backend`: Erasure coding engine (`jerasure`, `isa_l`, or `liberasurecode`).
- `chunk_size`: The boundary at which erasure coding is triggered (default: 32MB).

## 4. Development & Build

### Build Instructions
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Verification
Run the comprehensive test suite to verify the Reed-Solomon implementation and full write/read lifecycle:
```bash
cd build
./test_erasurecoder      # Unit tests for RS logic
./test_plugin_simulation # Integration test for full lifecycle
```

## License
Distributed under the **BSD 3-Clause License**. See the `LICENSE.md` file for details.
