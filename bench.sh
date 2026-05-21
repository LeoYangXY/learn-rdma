#!/bin/bash
cd /home/leo/RDMA-Tutorial
rm -f *.log

./build/rdma-tutorial local.config 54321 server &
SERVER_PID=$!
sleep 2
./build/rdma-tutorial local.config 54321 client
wait $SERVER_PID 2>/dev/null

echo "=== BENCHMARK RESULTS ==="
grep throughput "server[0].log" 2>/dev/null || echo "  server: no data"
grep throughput "client[0].log" 2>/dev/null || echo "  client: no data"
rm -f *.log
