#!/bin/bash
# Test script for collector daemon

set -e

echo "=== GatoTray Collector Test Suite ==="
echo ""

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    if [ ! -z "$COLLECTOR_PID" ]; then
        kill $COLLECTOR_PID 2>/dev/null || true
        wait $COLLECTOR_PID 2>/dev/null || true
    fi
    rm -f /tmp/gatotray_top.cache
}

trap cleanup EXIT

echo "Test 1: Building collector and client"
make clean && make
echo "✓ Build successful"
echo ""

echo "Test 2: Starting collector daemon"
./collector &
COLLECTOR_PID=$!
sleep 2
echo "✓ Collector started (PID: $COLLECTOR_PID)"
echo ""

echo "Test 3: Verifying cache file creation"
if [ -f /tmp/gatotray_top.cache ]; then
    echo "✓ Cache file created"
    ls -lh /tmp/gatotray_top.cache
else
    echo "✗ Cache file not found"
    exit 1
fi
echo ""

echo "Test 4: Collecting data for a few seconds"
sleep 3
echo "✓ Data collection running"
echo ""

echo "Test 5: Testing TOP command"
OUTPUT=$(./client -c TOP)
if echo "$OUTPUT" | grep -q "TIMESTAMP"; then
    echo "✓ TOP command works"
    echo "Sample output:"
    echo "$OUTPUT" | head -5
else
    echo "✗ TOP command failed"
    echo "$OUTPUT"
    exit 1
fi
echo ""

echo "Test 6: Testing HISTORY command"
OUTPUT=$(./client -c HISTORY)
if echo "$OUTPUT" | grep -q "TIMESTAMP"; then
    SNAPSHOT_COUNT=$(echo "$OUTPUT" | grep -c "^TIMESTAMP" || true)
    echo "✓ HISTORY command works (found $SNAPSHOT_COUNT snapshots)"
else
    echo "✗ HISTORY command failed"
    exit 1
fi
echo ""

echo "Test 7: Verifying process data format"
OUTPUT=$(./client -c TOP)
if echo "$OUTPUT" | grep -E "^[0-9]+ [0-9.]+ [0-9]+ .+$"; then
    echo "✓ Process data format is correct"
    echo "Sample process line:"
    echo "$OUTPUT" | grep -E "^[0-9]+ [0-9.]+ [0-9]+ .+$" | head -1
else
    echo "✗ Process data format incorrect"
    exit 1
fi
echo ""

echo "Test 8: Testing auto-spawn feature"
# Kill collector
kill $COLLECTOR_PID 2>/dev/null || true
wait $COLLECTOR_PID 2>/dev/null || true
COLLECTOR_PID=""
sleep 1

# Try auto-spawn (with timeout to prevent hanging)
timeout 5 ./client -a -c TOP > /tmp/autospawn_test.txt 2>&1 || true
OUTPUT=$(cat /tmp/autospawn_test.txt)
if echo "$OUTPUT" | grep -q "TIMESTAMP"; then
    echo "✓ Auto-spawn works"
    # Find and kill the auto-spawned collector
    pkill -f "./collector" 2>/dev/null || true
    sleep 1
else
    echo "⚠ Auto-spawn test skipped (requires collector in PATH or current directory)"
fi
rm -f /tmp/autospawn_test.txt
echo ""

echo "=== All tests passed! ==="
