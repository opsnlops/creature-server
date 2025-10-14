# Testing the Cooperative Scheduler

Quick reference guide for testing animation interrupts.

## Prerequisites

1. **Start server with cooperative scheduler:**
   ```bash
   cd build/
   ./creature-server --scheduler cooperative
   ```

2. **Get an animation ID** (from another terminal):
   ```bash
   curl http://localhost:8080/api/v1/animation | jq '.data[0].id'
   ```

## Quick Tests

### Using the Test Script (Recommended)

```bash
# Interactive mode - shows available animations
./docs/test-interrupt.sh

# Basic interrupt (no playlist resume)
./docs/test-interrupt.sh YOUR_ANIMATION_ID

# Interrupt with playlist resume
./docs/test-interrupt.sh YOUR_ANIMATION_ID 1 true

# Interrupt on different universe
./docs/test-interrupt.sh YOUR_ANIMATION_ID 2
```

### Using curl Directly

```bash
# Basic interrupt
curl -X POST http://localhost:8080/api/v1/animation/interrupt \
  -H "Content-Type: application/json" \
  -d '{
    "animation_id": "YOUR_ANIMATION_ID",
    "universe": 1
  }'

# Interrupt with playlist resume
curl -X POST http://localhost:8080/api/v1/animation/interrupt \
  -H "Content-Type: application/json" \
  -d '{
    "animation_id": "YOUR_ANIMATION_ID",
    "universe": 1,
    "resumePlaylist": true
  }'
```

## Expected Behaviors

### ✅ Success (HTTP 200)
```json
{
  "status": "success",
  "code": 200,
  "message": "Animation interrupt scheduled successfully"
}
```

**Server logs will show:**
```
[info] REST API: interrupting universe 1 with animation 507f...
[info] SessionManager: interrupting playback on universe 1 with animation 'My Animation'
[info] SessionManager: interrupt animation 'My Animation' scheduled on universe 1
```

### ❌ Wrong Scheduler (HTTP 400)
```json
{
  "status": "error",
  "code": 400,
  "message": "Animation interrupts require the cooperative scheduler. Start server with --scheduler cooperative"
}
```

**Fix:** Restart server with `--scheduler cooperative`

### ❌ Animation Not Found (HTTP 404)
The animation service will return 404 if the animation doesn't exist in the database.

**Fix:** Use a valid animation ID from `GET /api/v1/animation`

### ❌ Scheduling Failed (HTTP 500)
```json
{
  "status": "error",
  "code": 500,
  "message": "Failed to schedule interrupt animation: <details>"
}
```

**Check:** Server logs for detailed error information

## Observability

### View Traces in Honeycomb

If you have Honeycomb configured, search for:

```
service.name = "creature-server"
  AND name = "POST /api/v1/animation/interrupt"
```

**Key attributes to filter by:**
- `http.status_code` - Response code
- `universe` - Universe number
- `animation.id` - Animation ID
- `resume_playlist` - Whether resume was requested
- `error.type` - Error type (if failed)

### Server Logs

Key log lines to watch for:
```
[info] REST API: interrupting universe X with animation Y (resume: true/false)
[info] SessionManager: interrupting playback on universe X
[info] SessionManager: marked playlist on universe X as interrupted (resume: true/false)
[info] SessionManager: interrupt animation scheduled on universe X
```

## Testing Scenarios

### Scenario 1: Interrupt Nothing
**Setup:** No animation playing
**Action:** Send interrupt request
**Expected:** Animation plays normally (nothing to interrupt)

### Scenario 2: Interrupt Single Animation
**Setup:** Start an animation playing
**Action:** Send interrupt request
**Expected:** Current animation stops, interrupt animation plays

### Scenario 3: Interrupt Playlist (No Resume)
**Setup:** Start a playlist playing
**Action:** Send interrupt with `resumePlaylist: false`
**Expected:** Playlist stops, interrupt plays, playlist does NOT resume

### Scenario 4: Interrupt Playlist (With Resume)
**Setup:** Start a playlist playing
**Action:** Send interrupt with `resumePlaylist: true`
**Expected:** Playlist pauses, interrupt plays, playlist state saved for future resume

### Scenario 5: Wrong Scheduler
**Setup:** Server running with `--scheduler legacy` (default)
**Action:** Send interrupt request
**Expected:** HTTP 400 with clear error message

## Troubleshooting

### "Connection refused"
- Ensure server is running: `ps aux | grep creature-server`
- Check port 8080 is not in use: `lsof -i :8080`

### "Animation not found"
- List animations: `curl http://localhost:8080/api/v1/animation`
- Verify database connection in server logs

### Interrupt doesn't seem to work
- Check scheduler type: Server should log "using cooperative animation scheduler"
- Check logs for cancellation messages
- Verify universe has active playback before interrupt

### No logs appearing
- Server default log level is debug
- Check for errors in server startup
- Verify server is actually receiving the request: `curl http://localhost:8080/api/v1/animation`

## Next Steps

- See [cooperative-scheduler.md](cooperative-scheduler.md) for architecture details
- View [test-interrupt.sh](test-interrupt.sh) source for API examples
- Check OpenAPI docs at `http://localhost:8080/swagger/ui`
