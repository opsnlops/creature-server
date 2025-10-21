# CLAUDE.md

**For general project architecture, build instructions, and data models, see [AGENTS.md](AGENTS.md).**

This file provides Claude Code-specific guidance and workflow tips for this repository.

## Quick Start for Claude Code

1. **Read AGENTS.md first** - Contains all architectural decisions and project structure
2. **Build command**: `./local_build.sh` (from project root)
3. **Test command**: `cd build && ./creature-server-test`
4. **Code format**: Always run `clang-format` on modified C++ files

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