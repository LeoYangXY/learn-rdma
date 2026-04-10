# RDMA-Tutorial
This project presents an example based tutorial on RDMA based programming. A more detailed 
discussion can be found on the [Wiki](https://github.com/jcxue/RDMA-Tutorial/wiki) page.

## Hardware and software requirement
 * Mellanox HCAs
 * GNU make
 * gcc-4.4
 * Mellanox OFED 3.3

## How to use

### build project
Simply use ```make``` to build the release version or ```make debug``` to build the 
debug version.

### navigate through examples
The project contains 4 examples. Details of the examples can be found on the 
[Wiki](https://github.com/jcxue/RDMA-Tutorial/wiki) page. The code of the examples
are organized as git commits. Simply do ```git log --oneline``` to find the commit version number 
corresponding to the examples you are looking for.

## Contact

Jiachen Xue (jcxue.work@gmail.com)

---

## 中文阅读指引：RDMA Echo 全链路代码导读

### 项目概述

这是一个基于 **RC (Reliable Connection) + Send/Recv（双边操作）** 的 RDMA Echo 程序。  
Client 发送消息给 Server，Server 原样回送（Echo），如此循环 1000 万次，最终统计吞吐率。

### RDMA 通信链路四个阶段

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

按照 RDMA 通信链路的执行顺序，推荐依次阅读以下文件：

#### 第一步：了解全局入口和配置

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 1 | `main.c` | 程序入口 | 理解整体执行流程：解析配置 → 创建资源 → 运行负载 → 清理 |
| 2 | `debug.h` | 调试/日志宏 | 理解 `check()` 宏的 goto error 错误处理模式，后续所有文件都在用 |
| 3 | `config.h` / `config.c` | 配置解析 | 了解 `ConfigInfo` 结构体各字段含义即可，快速跳过 |

#### 第二步：理解 RDMA 核心概念（重点）

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 4 | **`ib.h`** | RDMA 核心头文件 | **最重要！** 文件开头有完整的 RDMA 链路概览和数据流图，建议反复阅读 |
| 5 | **`ib.c`** | QP 状态转换 + 发送/接收 | QP 三步状态转换（RESET→INIT→RTR→RTS）每个参数的含义；post_send / post_srq_recv 的完整数据链路 |

#### 第三步：理解资源创建和连接建立

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 6 | **`setup_ib.h`** | 资源结构体 | IBRes 结构体中每个字段的作用，以及资源依赖关系图 |
| 7 | **`setup_ib.c`** | 资源创建 + 连接建立 | `setup_ib()`: 10 步资源创建流程；`connect_qp_server/client()`: TCP 交换 QP 信息 + QP 状态转换 |
| 8 | `sock.h` / `sock.c` | TCP 辅助通道 | 为什么需要带外通道；QP 信息的字节序转换 |

#### 第四步：理解数据传输逻辑

| 顺序 | 文件 | 内容 | 阅读重点 |
|------|------|------|---------|
| 9 | **`server.c`** | Server 端 Echo | 预投递 Recv WR → 发 START → 主循环（poll CQ → Echo 回送 → 补充 Recv WR）→ 发 STOP |
| 10 | **`client.c`** | Client 端 Echo | 预投递 Recv WR → 等 START → 发起首批 Send → 主循环（收到 Echo → 再发一条 → 补充 Recv WR）→ 收 STOP |

### Echo 数据流示意

```
Client                                    Server
  │                                          │
  │  预投递 Recv WR                          │  预投递 Recv WR
  │                                          │
  │         ← SEND(imm=START) ──────────     │  通知 Client 可以开始
  │                                          │
  │  ── SEND(msg, imm=rank) ──────────→      │  Client 主动发起
  │                                          │  Server 收到, Echo 回送
  │         ← SEND(msg, imm=rank) ─────      │
  │  收到回送, 立即再发一条                    │
  │  ── SEND(msg, imm=rank) ──────────→      │
  │                                          │
  │       ... 循环 1000万次 ...               │
  │                                          │
  │         ← SEND(imm=STOP) ─────────      │  Server 发停止信号
  │  退出                                    │  退出
```

### 关键概念速查

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
| **LID (Local Identifier)** | IB 子网中的端口地址，类似 MAC 地址 | `setup_ib.c` 中 `port_attr.lid` |
| **imm_data** | Send with Immediate 携带的 32 位附加数据 | `ib.h` 中 `MsgType`，`ib.c` 中 `post_send` |

### 本项目的局限

本项目仅覆盖了 **RC + Send/Recv（双边操作）**，未涉及：
- **RDMA Read/Write（单边操作）**：对端 CPU 完全不参与，这才是 RDMA 的真正杀手锏
- **UD (Unreliable Datagram)**：一个 QP 可发给任意对端，类似 UDP
- **RDMA Atomic**：远程原子操作（CAS、Fetch&Add）
- **事件通知模式**：本项目只用了 busy polling
