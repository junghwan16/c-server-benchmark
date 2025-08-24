#!/bin/bash

# Quick test script for all servers

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== Testing All Servers ===${NC}"
echo

# Build if needed
if [ ! -f "./build/thread_http" ] || [ ! -f "./build/kqueue_http" ] || [ ! -f "./build/aio_http" ]; then
    echo -e "${YELLOW}Building servers...${NC}"
    make clean && make all
fi

# Function to test a server
test_server() {
    local name=$1
    local binary=$2
    
    echo -e "${GREEN}Testing $name server...${NC}"
    
    # Start server
    $binary > /dev/null 2>&1 &
    local pid=$!
    sleep 2
    
    # Check if running
    if ! kill -0 $pid 2>/dev/null; then
        echo -e "${RED}✗ $name failed to start${NC}"
        return 1
    fi
    
    # Test with curl
    if curl -s http://localhost:8080/index.html | grep -q "C Server Benchmark"; then
        echo -e "${GREEN}✓ $name basic test passed${NC}"
    else
        echo -e "${RED}✗ $name basic test failed${NC}"
    fi
    
    # Simple load test with ab if available
    if command -v ab &> /dev/null; then
        echo -n "  Load test (100 connections): "
        if ab -n 1000 -c 100 -t 2 http://localhost:8080/index.html 2>&1 | grep -q "Requests per second"; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
        fi
    fi
    
    # Kill server
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    
    echo
    sleep 2
}

# Test each server
test_server "Thread Pool" "./build/thread_http"
test_server "Kqueue" "./build/kqueue_http"
test_server "Select/AIO" "./build/aio_http"

echo -e "${GREEN}=== All Tests Complete ===${NC}"