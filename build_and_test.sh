#!/bin/bash
set -e

cd "$(dirname "$0")"

# ========================================
# 环境搭建（WSL 重启后需要重新执行）
# ========================================
setup_env() {
    echo "=== Setting up Soft-RoCE environment ==="

    # 加载 crc32 算法（RXE 计算 ICRC 必需，注意是 crc32 不是 crc32c）
    # WSL 重建后内核 crypto 不会自动加载 crc32，缺这个会导致
    # rdma link add 失败：rxe_icrc_init: failed to init crc32 algorithm err: -2
    sudo modprobe crc32_generic 2>/dev/null || true
    sudo modprobe libcrc32c   2>/dev/null || true
    sudo modprobe crc32c-intel 2>/dev/null || true
    sudo modprobe crc32c_generic 2>/dev/null || true

    # 创建 veth pair（忽略已存在错误）
    sudo ip link add veth0 type veth peer name veth1 2>/dev/null || true
    sudo ip addr add 10.0.0.1/24 dev veth0 2>/dev/null || true
    sudo ip addr add 10.0.0.2/24 dev veth1 2>/dev/null || true

    # 启用 IPv6（RXE 需要 link-local 地址生成 GID）
    sudo sysctl -qw net.ipv6.conf.veth0.addr_gen_mode=0
    sudo ip link set veth0 down
    sudo ip link set veth0 up
    sudo ip link set veth1 up
    sleep 2

    # 加载 RXE 模块并创建设备
    sudo modprobe rdma_rxe
    sudo rdma link add rxe0 type rxe netdev veth0 2>/dev/null || true

    # 添加 hosts 映射（如果不存在）
    grep -q "server1" /etc/hosts || sudo bash -c 'echo "10.0.0.1 server1" >> /etc/hosts'
    grep -q "client1" /etc/hosts || sudo bash -c 'echo "10.0.0.1 client1" >> /etc/hosts'

    # 验证
    if ibv_devinfo 2>&1 | grep -q "rxe0"; then
        echo "=== Soft-RoCE ready ==="
    else
        echo "ERROR: Failed to setup RXE device"
        exit 1
    fi
}

# ========================================
# 编译
# ========================================
build() {
    if [ "$1" = "debug" ]; then
        echo "=== Building (Debug) ==="
        cmake -B build -DCMAKE_BUILD_TYPE=Debug
    else
        echo "=== Building (Release) ==="
        cmake -B build -DCMAKE_BUILD_TYPE=Release
    fi
    cmake --build build
}

# ========================================
# 测试
# ========================================
test_run() {
    rm -f *.log

    echo "=== Starting Server ==="
    ./build/rdma-tutorial local.config 12345 server &
    SERVER_PID=$!
    sleep 1

    echo "=== Starting Client ==="
    ./build/rdma-tutorial local.config 12345 client
    CLIENT_EXIT=$?

    wait $SERVER_PID 2>/dev/null
    SERVER_EXIT=$?

    echo ""
    echo "=== Results ==="
    echo "Server log:"
    cat server\[0\].log 2>/dev/null | grep -E "throughput|ERROR" || echo "  (no output)"
    echo "Client log:"
    cat client\[0\].log 2>/dev/null | grep -E "throughput|ERROR" || echo "  (no output)"

    rm -f *.log

    if [ $CLIENT_EXIT -eq 0 ] && [ $SERVER_EXIT -eq 0 ]; then
        echo ""
        echo "=== PASS ==="
    else
        echo ""
        echo "=== FAIL (server=$SERVER_EXIT, client=$CLIENT_EXIT) ==="
        exit 1
    fi
}

# ========================================
# Main
# ========================================
case "${1:-all}" in
    setup)
        setup_env
        ;;
    build)
        build "$2"
        ;;
    test)
        test_run
        ;;
    all|"")
        setup_env
        build "$2"
        test_run
        ;;
    *)
        echo "Usage: $0 [setup|build|test|all] [debug]"
        exit 1
        ;;
esac
