# RDMA Echo Tutorial

## 这个项目是什么

**用 `libibverbs`（用户态 RDMA API）直接操控 RDMA 网卡，做了一个 Echo 乒乓测试来测量吞吐率。**

它不经过内核网络协议栈，不走 TCP/IP，数据直接从用户态缓冲区 → 网卡 → 对端网卡 → 对端用户态缓冲区。

类比：就像你用 `socket()` + `send()` + `recv()` 写了个 TCP Echo 程序来测 TCP 性能一样，只不过这里把 TCP/socket 换成了 RDMA/verbs。

---

## 快速开始（一键脚本）

```bash
cd /home/leo/RDMA-Tutorial

# 一键：环境搭建 + 编译 + 测试（需要 sudo 密码）
./build_and_test.sh

# 或分步执行：
./build_and_test.sh setup    # 仅搭建 Soft-RoCE 环境（WSL 重启后需要）
./build_and_test.sh build    # 仅编译（Release）
./build_and_test.sh build debug  # 编译 Debug 版本
./build_and_test.sh test     # 仅运行测试
```

---

## 手动编译

```bash
cd /home/leo/RDMA-Tutorial

# Release 模式（默认，有优化）
cmake -B build
cmake --build build

# 或 Debug 模式（有调试输出，打印每次 ops_count）
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

编译产物在 `build/rdma-tutorial`，项目根目录保持干净。

如果修改了代码，只需重新 `cmake --build build` 即可（增量编译）。

---

## 手动运行

```bash
# 终端1: 启动 Server
./build/rdma-tutorial local.config 12345 server

# 终端2: 启动 Client
./build/rdma-tutorial local.config 12345 client
```

两个进程跑起来后会自动完成 Echo 测试，最后输出吞吐率，然后退出。

输出示例：
```
thread[0]: throughput = 0.000545 (Mops/s)

================ Run Finished ================
```

---

## 跑起来后发生了什么（完整时间线）

```
时间线
═══════════════════════════════════════════════════════════════

  Server                              Client
  ──────                              ──────

  1. 打开 rxe0 设备
  2. 分配 PD、注册 MR、创建 CQ/SRQ/QP
  3. TCP 监听端口 12345
                                      1. 同上（打开设备、分配资源）
                                      2. TCP 连接到 server1:12345

  ─── TCP 交换 QP 信息 (lid + qp_num + gid) ───

  4. QP: RESET → INIT → RTR → RTS     3. QP: RESET → INIT → RTR → RTS
  5. TCP 同步确认后关闭 TCP             4. 同上

  ═══ 此时 RDMA 连接建立完成，TCP 不再使用 ═══

  6. 预投递 Recv WR 到 SRQ             5. 预投递 Recv WR 到 SRQ
  7. 发送 START ──────────────────────→ 6. 收到 START
                                      7. 发送第一批 Echo 消息（4条）

  ─── 主循环（乒乓球模式）───

  8. 收到消息 ←───────────────────────
     原样回送 ──────────────────────→  8. 收到 Echo 回送
                                         再发一条新消息
  ← ─────────────────────────────────

  ... 循环 N 次 ...

  9. 发送 STOP ─────────────────────→  9. 收到 STOP
  10. 输出吞吐率，退出                  10. 输出吞吐率，退出
```

---

## 项目在整个技术栈中的位置

| 层级 | 内容 | 本项目在哪 |
|------|------|-----------|
| 应用层 | KV 存储、RPC 框架 | **这里：Echo 延迟/吞吐测试** |
| Verbs API | ibv_post_send、ibv_poll_cq | ✅ 就是用这层 API 编程 |
| 驱动层 | mlx5_ib / rdma_rxe | 本地环境用 rdma_rxe（软件模拟） |
| 硬件层 | Mellanox ConnectX 网卡 | 本地无硬件，用 Soft-RoCE 代替 |

---

## 为什么用 RDMA 而不用 Socket

| | TCP Socket | RDMA Verbs (本项目) |
|---|---|---|
| 数据路径 | App → 内核 → 协议栈 → 网卡 | App → 网卡 (绕过内核) |
| 延迟 | ~10-50μs | ~1-2μs |
| CPU 占用 | 高（内核拷贝+中断） | 低（零拷贝+用户态轮询） |
| 编程复杂度 | 简单 | 很复杂 |

---

## 本地测试环境（Soft-RoCE）

本项目在 WSL2 上使用 **Soft-RoCE (RXE)** 纯软件模拟 RDMA 网卡。
完整数据通路如下（server 与 client 同机不同进程）：

```
   同一个 WSL 内核 / 同一台机器
   ┌──────────────────────────────────────────────────────────┐
   │                                                          │
   │   server 进程                    client 进程             │
   │   (rdma-tutorial server)        (rdma-tutorial client)   │
   │       │                              │                   │
   │       │ ibv_post_send/recv           │                   │
   │       ▼                              ▼                   │
   │  ┌─────────────────────────────────────────┐             │
   │  │   rdma_rxe.ko   (Soft-RoCE 内核模块)    │             │
   │  │   - 把 RDMA Verbs 翻译成 RoCE v2 报文   │             │
   │  │   - 自己算 ICRC、组 UDP 包              │             │
   │  └─────────────────────────────────────────┘             │
   │       │ UDP/IP 报文（目的端口 4791 = RoCE v2）           │
   │       ▼                                                  │
   │  ┌─────────────────────────────────────────┐             │
   │  │      内核网络栈 (IP / UDP)               │             │
   │  └─────────────────────────────────────────┘             │
   │       │                              ▲                   │
   │       ▼                              │                   │
   │   ┌────────┐                     ┌────────┐              │
   │   │ veth0  │ ◄──── veth pair ───►│ veth1  │              │
   │   │10.0.0.1│   (内核里的虚拟线缆) │10.0.0.2│              │
   │   └────────┘                     └────────┘              │
   │                                                          │
   └──────────────────────────────────────────────────────────┘
```

**注意一个微妙点**：因为 10.0.0.1 / 10.0.0.2 都是本机 IP，
`ip route get` 会显示二者都是 `local ... dev lo`，
所以 RoCE 报文实际走的是 **内核 loopback 路径**，veth pair
主要负责"提供 IP + 给 rxe0 一个可绑定的网卡"，并非真正承载数据。

### 环境搭建命令

```bash
# 创建 veth pair
sudo ip link add veth0 type veth peer name veth1
sudo ip addr add 10.0.0.1/24 dev veth0
sudo ip addr add 10.0.0.2/24 dev veth1
sudo ip link set veth0 up
sudo ip link set veth1 up

# 加载 RXE 内核模块，创建 Soft-RoCE 设备
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev veth0

# /etc/hosts 中添加
# 10.0.0.1 server1
# 10.0.0.1 client1
```

### Soft-RoCE vs 真实硬件

| | 真实 RDMA 网卡 | Soft-RoCE (本地) |
|---|---|---|
| 数据搬运 | HCA 硬件 DMA | CPU 软件模拟 |
| 性能 | 微秒级延迟、百Gbps | 很慢 |
| API | libibverbs | **完全相同** |
| 内核模块 | mlx5_ib 等 | `rdma_rxe` |

**关键：API 完全一样**，在这上面学的代码，拿到有真实 RDMA 网卡的机器上不用改就能跑。

---

## 代码结构与阅读顺序

### RDMA 通信链路四阶段

```
阶段1: 资源创建        阶段2: 连接建立        阶段3: 数据传输        阶段4: 资源清理
 setup_ib.c            setup_ib.c             server.c / client.c    setup_ib.c
                       + sock.c               + ib.c
     │                     │                       │                     │
 打开设备              TCP交换QP信息           预投递Recv WR           按逆序销毁
 分配PD                QP状态转换              主循环:                 QP→SRQ→CQ
 注册MR                RESET→INIT             poll CQ                 →MR→PD→ctx
 创建CQ/SRQ/QP          →RTR→RTS              post Send/Recv
```

### 建议阅读顺序

#### 第一步：了解全局入口和配置

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 1 | `main.c` | 程序入口 | 整体执行流程：解析配置 → 创建资源 → 运行负载 → 清理 |
| 2 | `debug.h` | 调试/日志宏 | `check()` 宏的 goto error 错误处理模式 |
| 3 | `config.h` / `config.c` | 配置解析 | `ConfigInfo` 结构体各字段含义 |

#### 第二步：理解 RDMA 核心概念（重点）

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 4 | **`ib.h`** | 核心头文件 | 文件开头有完整的 RDMA 链路概览和数据流图 |
| 5 | **`ib.c`** | QP 状态转换 + 收发 | QP 三步状态转换的每个参数含义；post_send / post_srq_recv |

#### 第三步：理解资源创建和连接建立

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 6 | **`setup_ib.h`** | 资源结构体 | IBRes 中每个字段的作用 |
| 7 | **`setup_ib.c`** | 资源创建 + 连接 | `setup_ib()`: 10 步创建流程；`connect_qp_*()`: TCP 交换 + 状态转换 |
| 8 | `sock.h` / `sock.c` | TCP 辅助通道 | 为什么需要带外通道；QP 信息字节序转换 |

#### 第四步：理解数据传输逻辑

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 9 | **`server.c`** | Server 端 Echo | 预投递 → 发 START → 主循环 → 发 STOP |
| 10 | **`client.c`** | Client 端 Echo | 预投递 → 等 START → 首批 Send → 主循环 → 收 STOP |

---

## 关键概念速查

| 概念 | 含义 | 在代码中的位置 |
|------|------|---------------|
| **QP (Queue Pair)** | 发送队列(SQ) + 接收队列(RQ)，代表一个连接 | `setup_ib.c` 创建，`ib.c` 状态转换 |
| **CQ (Completion Queue)** | 收割已完成操作的队列 | `setup_ib.c` 创建，`server.c`/`client.c` 中 `ibv_poll_cq` 轮询 |
| **MR (Memory Region)** | 注册到 HCA 的内存，可被 DMA 访问 | `setup_ib.c` 中 `ibv_reg_mr` |
| **SRQ (Shared Receive Queue)** | 多个 QP 共享的接收队列 | `setup_ib.c` 创建，`ib.c` 中 `post_srq_recv` |
| **WR (Work Request)** | 提交给 QP/SRQ 的工作请求 | `ib.c` 中 `ibv_send_wr` / `ibv_recv_wr` |
| **WC (Work Completion)** | CQ 中返回的完成通知 | `server.c`/`client.c` 中 `ibv_wc` |
| **SGE (Scatter/Gather Element)** | 描述数据在内存中的位置 | `ib.c` 中 `ibv_sge` |
| **PD (Protection Domain)** | 资源隔离的安全边界 | `setup_ib.c` 中 `ibv_alloc_pd` |
| **LID (Local Identifier)** | IB 子网中的端口地址 | `setup_ib.c` 中 `port_attr.lid` |
| **GID (Global Identifier)** | RoCE 中的全局路由地址（类似 IP） | `setup_ib.c` 中 `ibv_query_gid` |
| **imm_data** | Send with Immediate 携带的 32 位附加数据 | `ib.h` 中 `MsgType`，`ib.c` 中 `post_send` |

---

## Debug 技巧

libibverbs 只返回一个 errno，不告诉你哪个字段错了。实用技巧：

```bash
# 1. 开启 RXE 内核日志（最有用）
echo 'module rdma_rxe +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
dmesg -w   # 实时看内核日志

# 2. 用 ibv_rc_pingpong 验证环境是否正常
ibv_rc_pingpong -d rxe0 -g 1 &         # server
ibv_rc_pingpong -d rxe0 -g 1 localhost  # client

# 3. 抓 RoCE 包
sudo tcpdump -i veth0 -nn udp port 4791

# 4. 查看设备信息
ibv_devinfo
rdma link show
cat /sys/class/infiniband/rxe0/ports/1/gids/1
```

---

## IB vs RoCE 的关键差异

本代码原本为真实 IB 网卡设计，在 Soft-RoCE 上运行需要注意：

| | InfiniBand | RoCE v2 (Soft-RoCE) |
|---|---|---|
| 路由方式 | LID（子网内） | GID + GRH（类似 IP） |
| QP→RTR 时 | `ah_attr.is_global = 0`, 只需 `dlid` | `ah_attr.is_global = 1`, 需要设 `grh.dgid` |
| MTU | 通常 4096 | 受底层网卡限制（veth=1500 → MTU_1024） |
| 需要交换的信息 | lid + qp_num | lid + qp_num + **gid** |

---

## 本项目的局限

仅覆盖 **RC + Send/Recv（双边操作）**，未涉及：
- **RDMA Read/Write（单边操作）**：对端 CPU 完全不参与，这才是 RDMA 的真正杀手锏
- **UD (Unreliable Datagram)**：一个 QP 可发给任意对端，类似 UDP
- **RDMA Atomic**：远程原子操作（CAS、Fetch&Add）
- **事件通知模式**：本项目只用了 busy polling

---

## 原项目

Fork from [jcxue/RDMA-Tutorial](https://github.com/jcxue/RDMA-Tutorial)
