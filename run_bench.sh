#!/bin/bash
cd /home/leo/RDMA-Tutorial
rm -f *.log
PORT=$((40000 + RANDOM % 10000))
./build/rdma-tutorial local.config $PORT server &
SERVER_PID=$!
sleep 3
./build/rdma-tutorial local.config $PORT client
wait $SERVER_PID 2>/dev/null || true
echo "=== RESULTS ==="
grep throughput "server[0].log" 2>/dev/null || echo "server: no data"
grep throughput "client[0].log" 2>/dev/null || echo "client: no data"
rm -f *.log
