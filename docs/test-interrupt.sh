#!/bin/bash
#
# Test script for the animation interrupt API
# Requires: curl, jq (optional, for pretty JSON output)
#

# Configuration
SERVER="http://localhost:8080"
ANIMATION_ID="${1:-}"
UNIVERSE="${2:-1}"
RESUME="${3:-false}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if jq is available
if command -v jq &> /dev/null; then
    JQ_CMD="jq ."
else
    JQ_CMD="cat"
fi

print_section() {
    echo -e "\n${YELLOW}=== $1 ===${NC}\n"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Usage information
if [ -z "$ANIMATION_ID" ]; then
    echo "Usage: $0 <animation_id> [universe] [resumePlaylist]"
    echo ""
    echo "Examples:"
    echo "  $0 507f1f77bcf86cd799439011              # Interrupt universe 1, no resume"
    echo "  $0 507f1f77bcf86cd799439011 1 true       # Interrupt universe 1, resume playlist"
    echo "  $0 507f1f77bcf86cd799439011 2            # Interrupt universe 2, no resume"
    echo ""
    echo "Available animations:"
    curl -s "$SERVER/api/v1/animation" | $JQ_CMD
    exit 1
fi

print_section "Testing Animation Interrupt API"

echo "Configuration:"
echo "  Server: $SERVER"
echo "  Animation ID: $ANIMATION_ID"
echo "  Universe: $UNIVERSE"
echo "  Resume Playlist: $RESUME"

# Test 1: Check if animation exists
print_section "1. Checking if animation exists"
RESPONSE=$(curl -s -w "\n%{http_code}" "$SERVER/api/v1/animation/$ANIMATION_ID")
HTTP_CODE=$(echo "$RESPONSE" | tail -n1)
BODY=$(echo "$RESPONSE" | sed '$d')

if [ "$HTTP_CODE" = "200" ]; then
    print_success "Animation found"
    echo "$BODY" | $JQ_CMD
else
    print_error "Animation not found (HTTP $HTTP_CODE)"
    echo "$BODY" | $JQ_CMD
    exit 1
fi

# Test 2: Send interrupt request
print_section "2. Sending interrupt request"

PAYLOAD=$(cat <<EOF
{
  "animation_id": "$ANIMATION_ID",
  "universe": $UNIVERSE,
  "resumePlaylist": $RESUME
}
EOF
)

echo "Request payload:"
echo "$PAYLOAD" | $JQ_CMD

RESPONSE=$(curl -s -w "\n%{http_code}" -X POST "$SERVER/api/v1/animation/interrupt" \
  -H "Content-Type: application/json" \
  -d "$PAYLOAD")

HTTP_CODE=$(echo "$RESPONSE" | tail -n1)
BODY=$(echo "$RESPONSE" | sed '$d')

echo ""
echo "Response (HTTP $HTTP_CODE):"
echo "$BODY" | $JQ_CMD

case $HTTP_CODE in
  200)
    print_success "Interrupt scheduled successfully!"
    ;;
  400)
    print_error "Bad request - Check if cooperative scheduler is enabled"
    echo "Start server with: ./creature-server --scheduler cooperative"
    ;;
  500)
    print_error "Server error - Check server logs"
    ;;
  *)
    print_error "Unexpected response code: $HTTP_CODE"
    ;;
esac

print_section "Test Complete"
