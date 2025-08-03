# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Creature Server is a C++ server application for managing April's animatronic creatures. It provides a REST API and WebSocket interface for controlling animations, sounds, playlists, and hardware interactions via DMX/E1.31 lighting protocols.

## Build System

This project uses CMake with Ninja as the build generator and requires C++20. Dependencies are managed via FetchContent for most libraries.

### Key Build Commands

```bash
# Build oatpp dependencies first (required before main build)
./build_oatpp.sh

# Local debug build
./local_build.sh

# Manual build process
mkdir -p build/
cd build/
cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja ..
ninja

# Release build
cmake -DCMAKE_BUILD_TYPE=Release -G Ninja ..
ninja

# Run tests
cd build/
./creature-server-test
# Or via CTest
ctest
```

### Dependencies

External dependencies are fetched automatically via FetchContent:
- MongoDB C/C++ drivers (static linking)
- oatpp web framework (pre-built in externals/)
- spdlog for logging
- OpenTelemetry for observability
- SDL2 for audio
- Opus for audio encoding
- uvgRTP for RTP streaming
- nlohmann/json for JSON handling
- Google Test for unit testing

## Architecture

### Core Components

- **WebSocket Server** (`src/server/ws/`): oatpp-based REST API and WebSocket interface
- **Event Loop** (`src/server/eventloop/`): Main application event processing system
- **Database Layer** (`src/server/database.cpp`): MongoDB integration for data persistence
- **Animation System** (`src/server/animation/`): Animation playback and management
- **Audio/RTP** (`src/server/rtp/`): Real-time audio streaming via RTP/Opus with intelligent caching system
- **GPIO/Hardware** (`src/server/gpio/`): Hardware control interface
- **Metrics** (`src/server/metrics/`): Performance counters and status monitoring

### Data Models

Core entities in `src/model/`:
- **Creature**: Represents an animatronic creature with capabilities
- **Animation**: Motion sequences for creatures
- **Sound**: Audio files and playback metadata
- **Playlist**: Collections of animations/sounds for choreographed shows
- **Track**: Individual timeline elements within playlists

### Services Architecture

The application follows a service-oriented architecture:
- **Controllers** (`src/server/ws/controller/`): HTTP/WebSocket endpoints
- **Services** (`src/server/ws/service/`): Business logic layer
- **DTOs** (`src/server/ws/dto/`): Data transfer objects for API communication

## Development Workflow

### Testing

Unit tests are located in `tests/` and use Google Test framework. The test executable is `creature-server-test`.

### Linting and Code Quality

The project enforces strict compiler warnings:
- `-Wshadow` (overshadowed declarations)
- `-Wall -Wextra -Wpedantic`

**Code Formatting**: Always run `clang-format` on modified files. The project includes a `.clang-format` configuration file in the root directory that defines the required formatting style.

### Observability

Built-in OpenTelemetry integration for traces and metrics. The `ObservabilityManager` handles telemetry configuration and export.

## Key Libraries and Conventions

### Custom Libraries

- **CreatureVoicesLib** (`lib/CreatureVoicesLib/`): Voice synthesis integration
- **e131_service** (`lib/e131_service/`): DMX/E1.31 lighting protocol support
- **libe131** (`lib/libe131/`): Core E1.31 implementation

### Threading and Concurrency

Uses `moodycamel::ConcurrentQueue` for thread-safe message passing between components. The event loop pattern centralizes most application logic.

### Configuration

Command-line arguments handled via argparse. Database connection and server configuration managed through environment variables and config classes.

## Audio Cache System

The server includes an intelligent caching mechanism for pre-encoded Opus files to dramatically improve performance:

### Key Features
- **Performance**: Reduces Opus encoding time from dozens of seconds to <20ms for cache hits
- **Smart Invalidation**: Uses SHA-256 checksums, file modification times, and file sizes for cache validation
- **Storage Format**: Binary format with embedded metadata for fast validation
- **Location**: Stored in `.opus_cache/<hostname>/` subdirectory within the configured sounds directory
- **Multi-Machine Safe**: Each machine gets its own cache directory based on hostname to prevent conflicts on shared storage

### Implementation Details
- **Class**: `util::AudioCache` provides the caching interface
- **Integration**: `AudioStreamBuffer` automatically uses cache when available
- **File Format**: Custom binary format (17 channels × N frames) with metadata header
- **Error Handling**: Graceful fallback to direct encoding if cache unavailable or corrupt

### Performance Impact
- **Raspberry Pi 5**: Encoding time reduced from 30+ seconds to milliseconds
- **Cache Hit Rate**: High for repeated audio file usage in shows/playlists
- **Memory Usage**: Minimal - cache files stored on disk, loaded on demand

## Event Loop System

### Critical Timing Requirements

**⚠️ CRITICAL: The event loop MUST run every 1ms exactly.** This precise timing is fundamental to the entire system's operation and cannot be compromised. Any modifications to event loop code must preserve this exact 1ms interval.

### Event Loop Tracing System

The server implements intelligent selective tracing for the high-frequency event loop to provide error visibility while controlling observability costs:

### Key Features
- **Selective Sampling**: Configurable sampling rate for normal event loop frames (default: 0.1% = 1 in 1000)
- **Error-First Export**: All errors and exceptions are always traced regardless of sampling rate
- **Smart Export Logic**: Only exports traces when errors occur or random sampling criteria met
- **Rich Telemetry**: Frame numbers, events processed, queue sizes, timing data

### Configuration
- **Environment Variable**: `EVENT_LOOP_TRACE_SAMPLING` (0.0 to 1.0)
- **Command Line**: `--event-loop-trace-sampling 0.01` (for 1% sampling)
- **Default**: 0.001 (0.1% sampling rate)

### Implementation Details
- **Class**: `SamplingSpan` provides conditional export logic
- **Integration**: Event loop creates sampling spans for each frame iteration
- **Export Control**: Spans marked with sampling metadata for filtering in Honeycomb
- **Error Detection**: Automatic promotion to always-export on exceptions

### Cost Benefits
- **Volume Reduction**: 99.9% reduction in normal trace volume
- **Error Coverage**: 100% of errors and exceptions captured
- **Performance**: Minimal overhead (~1μs per frame for sampling decision)
- **Honeycomb Cost**: Dramatic reduction in trace ingestion costs

## Platform Notes

- **macOS**: Uses system SDL2 and resolv libraries
- **Debian Linux**: Additional UUID library dependency via pkg-config; OpenSSL 3.x (libssl3) required for audio cache hashing
- **Raspberry Pi**: Specialized build scripts (`pi4_build.sh`); runs on Raspberry Pi OS (Debian-based)

## Deployment

Docker-based deployment with multi-architecture support (AMD64/ARM64). GitHub Actions handle automated builds and container publishing.