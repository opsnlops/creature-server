# CLAUDE.md

**For general project architecture, build instructions, and data models, see [AGENTS.md](AGENTS.md).**

This file provides Claude Code-specific guidance and workflow tips for this repository.

## Quick Start for Claude Code

1. **Read AGENTS.md first** - Contains all architectural decisions and project structure
2. **Build command**: `./local_build.sh` (from project root)
3. **Test command**: `cd build && ./creature-server-test`
4. **Code format**: Always run `clang-format` on modified C++ files

## Workflow

### GitHub Issues for Features and Bugs (Required)
**When starting work on a feature or bug, open a GitHub Issue first** (`gh issue create`) and use it for tracking and notes — GitHub Issues play the role Jira does on a professional team:
- Capture the symptom, investigation notes, and root cause in the issue as you learn them — the issue is the durable record of *why*, not just *what*
- Reference the issue from commits and PRs (`Fixes #N` / `Refs #N`) so the fix links back to the analysis
- Close it when the fix lands on `main`
- The same rule applies to the console repo (creature-console)

## Tool Usage Guidelines

### When to Use the Task Tool with Explore Agent

Use the Explore agent for open-ended codebase exploration:
- ❌ **DON'T use for**: "Find the file that defines class Creature" (use Glob instead)
- ✅ **DO use for**: "How do errors from the client get handled?" (conceptual understanding)
- ✅ **DO use for**: "What is the codebase structure?" (broad exploration)
- ✅ **DO use for**: "Where are universe assignments tracked?" (multi-file reasoning)

Set thoroughness level appropriately:
- `"quick"` - Basic searches
- `"medium"` - Most tasks
- `"very thorough"` - Complex multi-location analysis

### File Operations

- **Reading files**: Use `Read` tool, not `cat` bash command
- **Editing files**: Use `Edit` tool, not `sed`/`awk`
- **Writing new files**: Use `Write` tool, not bash redirects
- **Finding files**: Use `Glob` tool for patterns, not `find`
- **Searching code**: Use `Grep` tool, not bash `grep`

### Parallel Tool Execution

Always run independent operations in parallel for efficiency:
```
# Good: Multiple file reads in one message
Read(file1) + Read(file2) + Read(file3)

# Bad: Sequential reads across multiple messages
Read(file1) → wait → Read(file2) → wait → Read(file3)
```

## Code Quality Reminders

### Always Use clang-format

This project has strict formatting requirements. Before making any commits or considering work complete:
```bash
clang-format -i src/path/to/modified/file.cpp
```

The `.clang-format` file in the project root defines the style.

### Compiler Warnings Are Errors

The build uses `-Wshadow -Wall -Wextra -Wpedantic`. Don't ignore warnings - fix them.

Common `-Wshadow` issues:
```cpp
// BAD - variable shadows parameter
void processCreature(const Creature& creature) {
    for (const auto& creature : creatures) { // ❌ Shadows parameter
        // ...
    }
}

// GOOD - different variable name
void processCreature(const Creature& creature) {
    for (const auto& c : creatures) { // ✅ No shadow
        // ...
    }
}
```

## Project-Specific Patterns

### Defensive Coding for DTOs

Always check for null DTOs after service calls:
```cpp
auto creatureDto = upsertCreature(jsonCreature, parentSpan);

// Defensive check
if (!creatureDto) {
    std::string errorMessage = "Invalid creature configuration provided";
    warn(errorMessage);
    OATPP_ASSERT_HTTP(false, Status::CODE_400, errorMessage.c_str());
}
```

### Span Handling

Always check if parent span is provided:
```cpp
if (!parentSpan) {
    warn("no parent span provided for Service.method, creating a root span");
}
auto span = creatures::observability->createOperationSpan("Service.method", parentSpan);
```

### Error Status Codes

- **400 Bad Request** - Client provided invalid data
- **404 Not Found** - Resource doesn't exist
- **500 Internal Server Error** - Server bug or unexpected condition

```cpp
// Client's fault (bad JSON, missing fields, etc.)
OATPP_ASSERT_HTTP(false, Status::CODE_400, "Invalid creature configuration");

// Server's fault (null pointer we didn't expect, internal error)
OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database returned null");
```

## Build Workflow

### Standard Build Process
```bash
# From project root
./local_build.sh

# Or manually:
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja ..
ninja
```

### After Making Changes

1. **Build**: `cd build && ninja`
2. **Format**: `clang-format -i` on modified files
3. **Test**: `./creature-server-test` (if relevant)
4. **Verify**: Check for compiler warnings

### Common Build Issues

- **oatpp errors**: Run `./build_oatpp.sh` first
- **Missing dependencies**: CMake FetchContent will download them automatically
- **Linker warnings about duplicates**: Safe to ignore (known issue with how dependencies are linked)

### Bumping the Version

Every deployable commit must bump the patch version — Debian packages can't collide and the apt repo rejects re-uploaded `.deb`s with a previously-used version. The version lives in **`VERSION.txt`** at the repo root, NOT in `CMakeLists.txt`. Bump it like:

```bash
echo "3.15.4" > VERSION.txt
```

That's the only file to touch. CMakeLists.txt reads `VERSION.txt` via `file(STRINGS …)` and feeds it into `project(VERSION …)`, which propagates into `Version.h` and the cpack-generated `.deb` filename.

Why a separate file: the Dockerfile's Phase 1 (the heavy FetchContent deps build) deliberately does NOT COPY `VERSION.txt`, so a version bump doesn't bust the dep-layer cache. A patch-bump-only commit is a ~2-minute CI run instead of ~15. See issue #18 + the comment block at the top of `CMakeLists.txt`.

If you're adding a new src/server/X/ directory, that DOES need a `CMakeLists.txt` edit (one new line under `file(GLOB serverFiles …)`) and will bust the Phase 1 cache one time. Adding a new .cpp inside an EXISTING globbed dir uses `CONFIGURE_DEPENDS` and doesn't need a CMake edit.

### Building .debs locally (faster than GHA)

GHA's Debian-package job takes ~15–25 min per arch. The local Docker build, reusing the same GHA cache, lands in roughly the same wall-clock but **runs in parallel with GHA** so the first one to finish unblocks the deploy. On April's M3 with plenty of RAM both archs can build at once.

**Prereqs:** Docker Desktop running (Rosetta enabled for amd64 cross-build speed).

**The full flow** (from project root, after `VERSION.txt` is bumped and the change is at least staged locally — the Docker context is whatever's on disk, not what's committed):

```bash
# Start both builds in parallel. The --cache-from scopes MUST match the
# GHA workflow (see .github/workflows/debian-package.yml) so the cached
# heavy deps layer gets pulled. Without this, every build is ~15 min cold.
docker buildx build --platform linux/amd64 --target package \
    --cache-from type=gha,scope=package-ubuntu-24.04 \
    -t creature-server-pkg-amd64 --load . &

docker buildx build --platform linux/arm64 --target package \
    --cache-from type=gha,scope=package-ubuntu-24.04-arm \
    -t creature-server-pkg-arm64 --load . &

wait

# Extract each .deb out of its image. The Dockerfile's `package` stage
# leaves the .deb at /package/ inside the image.
for arch in amd64 arm64; do
    docker rm -f temp-$arch 2>/dev/null
    docker create --name temp-$arch creature-server-pkg-$arch
    mkdir -p /tmp/pkg-$arch
    docker cp temp-$arch:/package/. /tmp/pkg-$arch/
    docker rm temp-$arch
    mv /tmp/pkg-$arch/*.deb out/debs/
    rmdir /tmp/pkg-$arch
done

ls -la out/debs/ | tail -3   # confirm the new .debs landed
```

**Notes:**
- `out/debs/` is gitignored — that's the canonical drop site so the deploy script can find them.
- The `--target package` flag stops the build at the package stage; we don't need the runtime stage locally.
- `--load` materializes the image into the local Docker daemon so `docker create` can pull files out.
- Filename format is `creature-server_<version>-<unix-timestamp>_<arch>.deb` — cpack generates the timestamp suffix automatically. Two builds back-to-back of the same version produce two distinct filenames, which the apt repo accepts.
- **First build after adding a new `src/server/X/` directory:** Phase 1 Docker cache busts once. Expect ~15 min for that build; subsequent ones return to ~2–5 min.
- **If a build errors out:** check that `docker buildx` is set up (`docker buildx ls` should show a `*` next to the active builder). On a fresh machine: `docker buildx create --use`.

To run only one arch (e.g. amd64-only because that's all the server needs right now), drop the arm64 lines.

## Session Continuity Notes

### Context for Next Session

**Recent Major Changes (October 2025):**
- Added `mouth_slot` (uint8_t) as required field in Creature model
- Separated universe assignment (runtime state) from creature config (persistent)
- Created `/api/v1/creature/register` endpoint for controller registration
- Removed old `/api/v1/creature` upload endpoint (replaced by register)
- Added `creatureUniverseMap` runtime cache for universe tracking

**Important Files Recently Modified:**
- `src/model/Creature.h` - Added mouth_slot field
- `src/model/Creature.cpp` - Updated conversions and validation
- `src/server/creature/helpers.cpp` - Added mouth_slot validation
- `src/server/ws/controller/CreatureController.h` - New register endpoint
- `src/server/ws/service/CreatureService.cpp` - Registration logic
- `src/server/main.cpp` - Added creatureUniverseMap initialization

**Key Architectural Principle:**
Controller's JSON file = source of truth for creature config. Database is just a cache. Universe is runtime-only state, not persisted.

## Common Tasks

### Adding a New DTO

1. Create file in `src/server/ws/dto/`
2. Use oatpp macros: `DTO_INIT`, `DTO_FIELD`, `DTO_FIELD_INFO`
3. Remember `#include OATPP_CODEGEN_BEGIN(DTO)` and `END`
4. Add to controller as `BODY_DTO(Object<YourDto>, request)`

### Adding a New Endpoint

1. Add to appropriate controller in `src/server/ws/controller/`
2. Use `ENDPOINT_INFO` for API documentation
3. Use `ENDPOINT` macro with HTTP method and path
4. Create request span for observability
5. Add HTTP attributes to span
6. Call service layer method
7. Handle exceptions with proper status codes
8. Schedule cache invalidation if needed

### Modifying Data Models

1. Update struct in `src/model/`
2. Update DTO in same file
3. Update `convertToDto()` and `convertFromDto()`
4. Update JSON parsing in `src/server/creature/helpers.cpp`
5. Update validation (required fields list)
6. Add to database queries if needed
7. Test with actual JSON payloads