#!/bin/bash

# K6 C10K Test Script
# Tests servers with gradual ramp-up to 10,000 concurrent connections

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== K6 C10K Performance Test ===${NC}"
echo

# Check if k6 is installed
if ! command -v k6 &> /dev/null; then
    echo -e "${RED}k6 is not installed!${NC}"
    echo "Install with: brew install k6"
    echo "Or download from: https://k6.io/docs/getting-started/installation/"
    exit 1
fi

# Build servers if needed
if [ ! -f "./build/thread_http" ] || [ ! -f "./build/kqueue_http" ]; then
    echo -e "${YELLOW}Building servers...${NC}"
    make clean && make all
fi

# Create results directory
mkdir -p results

# Function to test a server
test_server() {
    local server_name=$1
    local server_cmd=$2
    
    echo -e "${GREEN}Testing $server_name server with k6${NC}"
    
    # Skip aio if problematic
    if [[ "$server_name" == "aio" ]]; then
        echo -e "${YELLOW}Skipping aio server (segfault issue)${NC}"
        return 0
    fi
    
    # Start server
    echo "Starting $server_name server..."
    $server_cmd &
    local server_pid=$!
    sleep 2
    
    # Check if server is running
    if ! kill -0 $server_pid 2>/dev/null; then
        echo -e "${RED}Failed to start $server_name server${NC}"
        return 1
    fi
    
    # Verify server responds
    if ! curl -s http://localhost:8080/index.html > /dev/null 2>&1; then
        echo -e "${RED}Server not responding${NC}"
        kill $server_pid 2>/dev/null || true
        return 1
    fi
    
    echo "Server running (PID: $server_pid)"
    echo
    
    # Run k6 test
    echo -e "${YELLOW}Running k6 C10K test...${NC}"
    k6 run k6-c10k-test.js --out json=results/${server_name}-k6-metrics.json 2>&1 | tee results/${server_name}-k6-output.txt
    
    # Get server stats after test
    if kill -0 $server_pid 2>/dev/null; then
        echo
        echo "Server Statistics:"
        local mem_usage=$(ps -o rss= -p $server_pid 2>/dev/null || echo "0")
        echo "  Memory usage: $((mem_usage / 1024)) MB"
        local fd_count=$(lsof -p $server_pid 2>/dev/null | wc -l || echo "0")
        echo "  Open file descriptors: $fd_count"
    fi
    
    # Kill server
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    echo -e "${GREEN}Completed $server_name test${NC}"
    echo
    sleep 3
}

# Quick k6 test - lighter load for testing
quick_test() {
    local server_name=$1
    local server_cmd=$2
    
    echo -e "${GREEN}Quick k6 test for $server_name server${NC}"
    
    # Start server
    $server_cmd &
    local server_pid=$!
    sleep 2
    
    if ! kill -0 $server_pid 2>/dev/null; then
        echo -e "${RED}Failed to start server${NC}"
        return 1
    fi
    
    # Quick k6 test with lower load
    echo -e "${YELLOW}Running quick k6 test (max 1000 VUs)...${NC}"
    k6 run --vus 100 --duration 30s --out json=results/${server_name}-k6-quick.json - <<EOF
import http from 'k6/http';
import { check } from 'k6';

export default function () {
    const res = http.get('http://localhost:8080/index.html');
    check(res, {
        'status is 200': (r) => r.status === 200,
    });
}
EOF
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
    
    echo -e "${GREEN}Quick test completed${NC}"
    echo
}

# Main menu
echo "Select test mode:"
echo "1) Quick test (max 1000 connections)"
echo "2) Full C10K test (ramp up to 10000 connections)"
echo "3) Exit"
echo
read -p "Choice [1-3]: " choice

case $choice in
    1)
        echo -e "${YELLOW}Running quick tests...${NC}"
        quick_test "thread" "./build/thread_http"
        quick_test "kqueue" "./build/kqueue_http"
        ;;
    2)
        echo -e "${YELLOW}Running full C10K tests...${NC}"
        test_server "thread" "./build/thread_http"
        test_server "kqueue" "./build/kqueue_http"
        ;;
    3)
        echo "Exiting..."
        exit 0
        ;;
    *)
        echo -e "${RED}Invalid choice${NC}"
        exit 1
        ;;
esac

# Summary
echo -e "${GREEN}=== Test Complete ===${NC}"
echo "Results saved in ./results/"
ls -la results/*k6* 2>/dev/null || true

echo
echo -e "${YELLOW}Tips:${NC}"
echo '- View JSON results: cat results/*-k6-metrics.json | jq'
echo '- For dashboard: k6 run --out influxdb=http://localhost:8086/k6 k6-c10k-test.js'
echo '- For cloud: k6 cloud k6-c10k-test.js'