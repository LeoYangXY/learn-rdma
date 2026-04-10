# RDMA-Tutorial (CPU) vs DeepEP (GCU) 深度对比分析

## 一、结论先行

**DeepEP 不是重新实现了 Verbs API，而是绕过了 Verbs API，直接操作 RDMA 硬件。**

更准确地说：

| | RDMA-Tutorial (本仓库) | DeepEP (你的 GCU 代码) |
|---|---|---|
| **本质** | 标准 RDMA 用户态编程 | Kernel-initiated RDMA (IBGDA 技术) |
| **调用路径** | 用户进程 → libibverbs → 驱动 → HCA 硬件 | GCU Kernel → 直接写硬件寄存器 (WQE + Doorbell) |
| **类比** | 你写好快递单交给前台，前台叫快递员来取件 | 你自己走到快递员面前，把包裹塞他手里说"送！" |

它们的关系是：

```
                    标准 RDMA 软件栈
                    ┌─────────────────────────┐
 RDMA-Tutorial ──→  │  ibv_post_send()         │ ← 封装好的高层 API
                    │    ↓                     │
                    │  libibverbs 库            │
                    │    ↓                     │
                    │  内核驱动 (mlx5)          │
                    │    ↓                     │
                    │  构建 WQE → 写入 SQ       │ ← DeepEP 从这里开始自己干
                    │    ↓                     │
                    │  敲 Doorbell              │ ← DeepEP 也自己干
                    │    ↓                     │
                    │  HCA 硬件执行 DMA 传输    │ ← 两者最终走同一条路
                    └─────────────────────────┘

 DeepEP ──→ 跳过上面所有层，GCU kernel 直接从"构建 WQE"这一步开始
```

---

## 二、为什么 DeepEP 要绕过 Verbs API？

标准 Verbs API 的 `ibv_post_send()` 调用路径：

```
GCU Kernel 想发数据
    → 通知 CPU（中断/轮询）         ← 延迟！
    → CPU 调用 ibv_post_send()      ← 延迟！
    → libibverbs 构建 WQE            ← 延迟！
    → 写入 SQ + 敲 Doorbell
    → HCA 硬件开始搬运
```

这个 GCU→CPU→硬件 的往返，对于 MoE (Mixture of Experts) 的 token 级通信来说延迟太高了。DeepEP 的 MoE dispatch 需要在**每个推理 step** 中为**每个 token** 发送 RDMA 请求（可能几千个 token），如果每次都绕 CPU 一圈，延迟完全无法接受。

DeepEP 的解决方案 (IBGDA - InBand GDA)：

```
GCU Kernel 想发数据
    → 自己构建 WQE (esl_engine.send())
    → 自己写入 SQ (lare_write_wqe())
    → 自己敲 Doorbell (lare_ring_db())
    → HCA 硬件开始搬运

整个过程 CPU 完全不参与！
```

---

## 三、两者都是 RDMA，但层次完全不同

### 3.1 协议层面：完全相同

| 维度 | RDMA-Tutorial | DeepEP |
|------|---------------|--------|
| 传输协议 | RC (Reliable Connection) | RC (Reliable Connection) |
| 可靠性 | 硬件保证有序+可靠 | 硬件保证有序+可靠 |
| 线路上的数据包格式 | 标准 IB/RoCE 数据包 | 标准 IB/RoCE 数据包 |
| PSN 管理 | 硬件自动 | 硬件自动 |
| ACK/重传 | 硬件自动 | 硬件自动 |

**两者在网线上跑的数据包是一模一样的。** 如果你用抓包工具看，分不出哪个是 Verbs API 发的，哪个是 DeepEP 发的。

### 3.2 操作类型：不同

| | RDMA-Tutorial | DeepEP |
|---|---|---|
| **操作类型** | **Send/Recv (双边操作)** | **RDMA Write (单边操作)** |
| 接收端参与度 | 必须预投递 Recv WR | 接收端 CPU/GCU **完全不参与** |
| 接收端感知方式 | CQ 中出现 Recv WC | 自旋等待 flag/count 变为非零 |
| 接收端前置准备 | `post_srq_recv()` | 不需要任何准备 |

这是**最关键的区别之一**。

RDMA-Tutorial (Send/Recv)：
```
Server:  post_recv(buf)  →  等待 CQ 中出现 Recv WC  →  读 buf 中的数据
Client:  post_send(data) →  等待 CQ 中出现 Send WC  →  确认发送完成
```

DeepEP (RDMA Write)：
```
发送端:  直接指定远端内存地址，写入数据
接收端:  while (*flag == 0);  // 自旋等待发送端最后写入的标志位
```

DeepEP 不需要 `post_recv`，不需要 CQ，因为 RDMA Write 是发送端单方面把数据"塞"到接收端的指定内存位置。接收端唯一需要做的就是等着数据到达。

### 3.3 调用层面：完全不同

| 步骤 | RDMA-Tutorial (Verbs API) | DeepEP (直接操作硬件) |
|------|---------------------------|----------------------|
| 构建请求 | 填 `ibv_send_wr` 结构体 | 填 `lare_wrinfo` 结构体 |
| 提交请求 | `ibv_post_send(qp, &wr, &bad_wr)` | `lare_write_wqe()` 直接写 SQ 内存 |
| 触发硬件 | (隐含在 ibv_post_send 内部) | `lare_ring_db()` 手动敲门铃 |
| 确认完成 | `ibv_poll_cq()` 轮询 CQ | `while (*flag == 0)` 自旋等待 |

---

## 四、RDMA 资源对照表

### 4.1 资源创建阶段

| 资源 | RDMA-Tutorial 中的代码 | DeepEP 中的处理方式 |
|------|----------------------|-------------------|
| **设备上下文 (ctx)** | `ibv_open_device()` in setup_ib.c | host 端完成，kernel 不参与 |
| **保护域 (PD)** | `ibv_alloc_pd()` in setup_ib.c | host 端完成，kernel 不可见 |
| **内存区域 (MR)** | `ibv_reg_mr()` in setup_ib.c | host 端完成；kernel 中 `rdma_recv_x`/`rdma_x` 等缓冲区已注册好 |
| **完成队列 (CQ)** | `ibv_create_cq()` in setup_ib.c | **不存在！** 用 flag 自旋等待替代 |
| **共享接收队列 (SRQ)** | `ibv_create_srq()` in setup_ib.c | **不存在！** RDMA Write 不需要接收队列 |
| **队列对 (QP)** | `ibv_create_qp()` in setup_ib.c | host 端创建；kernel 中通过 `esl_endpoint` 结构体使用 |
| **QP 状态转换** | `modify_qp_to_rts()` in ib.c，手动走 RESET→INIT→RTR→RTS | host 端完成，kernel 拿到的 QP 已经是 RTS 状态 |
| **QP 信息交换** | TCP Socket 交换 LID+QPN in sock.c | host 端完成，`rdma_peer_base[]` 存对端基地址 |
| **LKey/RKey** | `ib_res.mr->lkey` 显式传递 | 隐式存在，已配置到 QP 硬件中 |

**关键发现：** DeepEP 的 kernel 代码中看不到任何资源创建操作。所有"阶段1"和"阶段2"的工作都在 host 端（CPU 侧）完成了。Kernel 只做"阶段3：数据传输"。

### 4.2 数据传输阶段

| 操作 | RDMA-Tutorial | DeepEP |
|------|---------------|--------|
| **描述数据位置** | `struct ibv_sge { addr, length, lkey }` | `lare_wrinfo { srcAddr, dstAddr, data_nelem }` |
| **描述发送操作** | `struct ibv_send_wr { wr_id, sg_list, opcode, imm_data }` | `lare_wrinfo { port, qp, fence, enableMsi }` |
| **提交发送** | `ibv_post_send(qp, &wr, &bad_wr)` | `lare_write_wqe()` 直接写 SQ + `lare_ring_db()` 敲门铃 |
| **提交接收** | `ibv_post_srq_recv(srq, &wr, &bad_wr)` | **不需要！** RDMA Write 不需要接收端预投递 |
| **确认发送完成** | `ibv_poll_cq()` 检查 Send WC | `lare_wait_done()` 或忽略（靠接收端 flag 间接确认） |
| **确认接收完成** | `ibv_poll_cq()` 检查 Recv WC | `while (*flag == 0)` 自旋等待发送端写入的 flag |

---

## 五、数据流对比

### 5.1 RDMA-Tutorial: Echo 模式 (Send/Recv)

```
Client                                    Server
  │                                          │
  │  (1) pre-post Recv WR 到 SRQ             │  (1) pre-post Recv WR 到 SRQ
  │                                          │
  │         ← SEND(imm=START) ────────       │  (2) 发 START 控制信号
  │                                          │
  │  (3) SEND(msg, imm=rank) ─────────→      │  (4) poll CQ → 收到 Recv WC
  │                                          │      Echo: SEND(msg) 发回来
  │  (5) poll CQ → 收到 Recv WC  ←────       │
  │      再发一条 SEND ───────────────→       │
  │                                          │
  │  ... 循环 1000 万次 ...                   │
  │                                          │
  │         ← SEND(imm=STOP) ─────────       │  (6) 发 STOP 控制信号

特点：
  - 双边操作：收发双方都要参与（post_send + post_recv）
  - 用 CQ 确认完成
  - 用 imm_data 传递控制信息
  - 一问一答的乒乓模式
```

### 5.2 DeepEP: MoE Dispatch + Combine (RDMA Write)

```
Rank 0 (发送端)                              Rank 1 (接收端)
  │                                            │
  │  (1) 统计每个 expert 收到多少 token         │  什么都不用做
  │                                            │
  │  (2) 量化: BF16 → FP8 + scale              │
  │                                            │
  │  (3) 原子抢 slot:                           │  接收缓冲区在 host 端
  │      atomic_add(counter, 1)                 │  就已经分配好了
  │                                            │
  │  (4) 计算远端地址:                           │
  │      peer_base[rank1] + offset              │
  │                                            │
  │  (5) RDMA Write 数据:                       │  ← 数据直接写入 Rank1 内存
  │      get_wqe_slots() → 分配 WQE 槽位        │     接收端 GCU 完全不知道！
  │      send()          → 构建 WQE 写入 SQ      │
  │      barrier()       → 内存屏障              │
  │      ringDb()        → 敲门铃               │
  │                                            │
  │  (6) RDMA Write count (fence=true):         │  ← count 到达（保证在数据之后）
  │      fence 保证 count 在所有数据之后到达      │
  │                                            │
  │                                            │  (7) while (*count == 0); 自旋等待
  │                                            │  (8) 解码 count，开始处理数据
  │                                            │  (9) pack 数据到紧凑缓冲区

特点：
  - 单边操作：只有发送端操作硬件，接收端只做自旋等待
  - 不用 CQ，用 flag/count 自旋等待
  - 不用 post_recv，接收端完全被动
  - 多对多的 All-to-All 模式（每个 rank 可能同时给所有其他 rank 发数据）
```

---

## 六、`ibv_post_send()` 内部干了什么 vs DeepEP 怎么自己干

这是理解两者关系的核心。`ibv_post_send()` 并不是魔法，它内部做的事情就是：

```c
// ibv_post_send 的简化伪代码 (以 mlx5 驱动为例)
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr, ...) {
    // 1. 根据 wr 构建硬件格式的 WQE
    build_wqe(qp->sq_buf + wqe_offset, wr);
    
    // 2. 内存屏障，确保 WQE 对硬件可见
    wmb();
    
    // 3. 敲 doorbell，通知硬件有新 WQE
    write_doorbell(qp->doorbell_reg, wqe_index);
    
    return 0;
}
```

DeepEP 做的正是同样的三步，只不过是在 GCU kernel 中执行：

```cpp
// DeepEP 的等价实现
// 1. 分配 WQE 槽位 (对应 ibv_post_send 内部的 wqe_offset 计算)
uint64_t wqe_idx = esl_engine.get_wqe_slots(qp_ptr, 1);

// 2. 构建 WQE 写入 SQ (对应 ibv_post_send 内部的 build_wqe)
esl_engine.send<...>(dst_ptr, src_ptr, qp_ptr, wqe_idx, size, fence);

// 3. 内存屏障 (对应 ibv_post_send 内部的 wmb)
__gcu_get_spr_barrier(BARRIER_MASK_L3);

// 4. 敲门铃 (对应 ibv_post_send 内部的 write_doorbell)
esl_engine.ringDb(qp_ptr, wqe_idx, 1);
```

**所以 DeepEP 不是"重新实现了 Verbs API"，而是"拆开了 Verbs API 的内部实现，在 GCU 端自己干"。**

Verbs API 是一个封装好的黑盒子，把"构建 WQE + 写 SQ + 敲 doorbell"封装成一个 `ibv_post_send()` 调用。DeepEP 把这个黑盒子拆开了，在 GCU kernel 中逐步手动执行。

---

## 七、QP 共享状态管理 —— DeepEP 独有的复杂性

RDMA-Tutorial 中，一个线程独占一个 QP 的操作权，不存在并发冲突。

但 DeepEP 的 dispatch 阶段，**多个 SM/subthread 可能同时往同一个 QP 发数据**（因为多个 token 可能发往同一个 rank）。这引入了一个 RDMA-Tutorial 完全不需要考虑的问题：**SQ 的并发访问控制**。

```
DeepEP 的 QP 共享状态 (qp_shared_state):

  postIdx      dbTouchIdx      doneIdx
  (已分配)      (已敲门铃)       (已完成)
     │              │              │
     ▼              ▼              ▼
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │ WQE│ WQE│ WQE│ WQE│    │    │    │    │  SQ (环形缓冲区, 256 槽位)
  └────┴────┴────┴────┴────┴────┴────┴────┘
  ▲                   ▲                   ▲
  已完成,可回收        等待敲门铃           可分配的空间

分配: atomic_add(postIdx, N)        → 抢 N 个槽位
敲铃: 等 dbTouchIdx == my_idx 后敲  → 保证门铃顺序
回收: atomic_max(doneIdx, waitId)   → 释放已完成的槽位
```

这套机制在 RDMA-Tutorial 中完全不存在——因为它是单线程、单 QP、一个个串行 post 的。

DeepEP 的 combine 阶段比较简单：每个 subthread 独占一个 QP，所以用 `get_wqe_slots_raw` / `ringDb_raw`（非原子版本），跳过等待和原子操作。

---

## 八、地址映射 —— 两种完全不同的思路

### RDMA-Tutorial: 不需要地址映射

Send/Recv 模式下，发送端不需要知道接收端的内存地址。发送端只需要指定"发送缓冲区在哪"，接收端通过 `post_recv` 指定"接收缓冲区在哪"。HCA 硬件自动将发送端的数据放入接收端预投递的 Recv WR 所指向的缓冲区。

### DeepEP: 精确的远程地址计算

RDMA Write 必须由发送端指定接收端的精确内存地址。DeepEP 使用"镜像布局"技巧：

```
所有 rank 的 RDMA 缓冲区布局完全相同：
  Rank 0:  [base_0] [expert_0_data] [expert_1_data] ... [count_array]
  Rank 1:  [base_1] [expert_0_data] [expert_1_data] ... [count_array]
  Rank 2:  [base_2] [expert_0_data] [expert_1_data] ... [count_array]

地址映射公式：
  remote_addr = rdma_peer_base[dst_rank] + (local_addr - rdma_buffer_ptr)

即：对端基地址 + 本地偏移量 = 对端实际地址
```

`rdma_peer_base[]` 数组在 host 初始化时通过 TCP 等带外通道交换，存入 `prims` 结构体传给 kernel。

---

## 九、完成通知机制对比

### RDMA-Tutorial: 标准 CQ 轮询

```c
// 轮询 CQ，取出已完成的 WC
n = ibv_poll_cq(cq, batch_size, wc_array);
for (i = 0; i < n; i++) {
    if (wc[i].opcode == IBV_WC_RECV) {
        // 收到消息，处理数据
        char *data = (char *)wc[i].wr_id;  // wr_id 就是缓冲区地址
        uint32_t sender = ntohl(wc[i].imm_data);  // 从 imm_data 获取发送者
    }
}
```

### DeepEP: flag/count 自旋等待

```cpp
// dispatch 接收端：等 count 到达
volatile int* ptr = (volatile int*)(rdma_recv_count + expert * num_ranks + src_rank);
int count = 0;
while ((count = *ptr) == 0);  // 自旋！直到发送端 RDMA Write 了 count
count = -count - 1;           // 解码负编码

// combine 接收端：等 flag 到达
while (*(volatile int*)(rdma_recv_flag + idx) == 0
       && (cost = clock64() - start) <= TIMEOUT);
```

**为什么不用 CQ？**
1. CQ 是 CPU 端的概念，GCU kernel 没有直接访问 CQ 的标准接口
2. RDMA Write 完成后本来也不会在**接收端**产生 CQ 事件（除非用 RDMA Write with Immediate）
3. 自旋等待 flag 更简单直接，在 GCU 端容易实现

---

## 十、三方同步机制 —— DeepEP 独有的精巧设计

RDMA-Tutorial 的同步很简单：Server 发 START → Client 开始 → Server 发 STOP → Client 停止。

DeepEP 的 dispatch 阶段有一个精巧的"三方同步"问题：**必须等所有数据都发完，才能发送 count。**

```
问题：发送端有多个 SM/subthread 同时工作，怎么知道"所有 token 都发完了"？

解决方案：完成计数器 (atomic_finish_counter_per_expert)

三方各自贡献：
  ① 初始化: +1024                    (SM 最后一个 subthread 负责)
  ② 统计:   +(1024 - token_count)    (倒数第二个 subthread 负责)
  ③ 发送:   每发一个 token +1          (所有 subthread 协作)
  
  最终值 = 1024 + (1024-count) + count = 2048 = 2 × FINISHED_SUM_TAG

等待条件：while (*counter != 2 * FINISHED_SUM_TAG);
含义："三方全部完成了，可以安全发送 count 了"
```

这种设计在 RDMA-Tutorial 中完全不需要——因为它是单线程串行的。

---

## 十一、总结对照表

| 维度 | RDMA-Tutorial (本仓库) | DeepEP (你的 GCU 代码) |
|------|----------------------|----------------------|
| **运行位置** | CPU 用户态进程 | GCU (类 GPU) kernel |
| **API 层次** | 标准 libibverbs (Verbs API) | 直接操作硬件 (IBGDA) |
| **传输协议** | RC (Reliable Connection) | RC (Reliable Connection) |
| **操作类型** | Send/Recv (双边) | RDMA Write (单边) |
| **资源创建** | 代码中显式完成 (setup_ib.c) | host 端完成，kernel 不参与 |
| **QP 状态转换** | 手动 RESET→INIT→RTR→RTS (ib.c) | host 端完成，kernel 拿到 RTS 状态的 QP |
| **信息交换** | TCP Socket 交换 LID+QPN (sock.c) | host 端完成，rdma_peer_base[] 传给 kernel |
| **提交发送** | `ibv_post_send()` 一个调用 | 手动分三步：分配WQE槽位 → 写WQE → 敲门铃 |
| **提交接收** | `ibv_post_srq_recv()` | **不需要** (RDMA Write 不需要接收端参与) |
| **完成确认** | `ibv_poll_cq()` 轮询 CQ | `while (*flag == 0)` 自旋等待 |
| **地址管理** | 不需要 (Send/Recv 模式) | 镜像布局 + `peer_base + offset` |
| **并发控制** | 无 (单线程) | 原子操作管理 SQ 的 postIdx/dbTouchIdx/doneIdx |
| **数据处理** | 无 (纯 Echo) | FP8 量化、BF16→FP32 转换、加权合并 |
| **应用场景** | RDMA 入门学习 | 生产级 MoE 推理通信 (DeepSeek) |
| **代码复杂度** | ~500 行 C | ~800 行 kernel C++ (不含 host 端) |
| **性能追求** | 教学为主 | 极致低延迟 (微秒级 token 分发) |

---

## 十二、一句话总结

> **RDMA-Tutorial 是标准 Verbs API 的"使用者"，通过封装好的 API 与 RDMA 硬件交互；
> DeepEP 是 Verbs API 的"替代者"，绕过 API 直接操作 RDMA 硬件的底层元素（WQE、SQ、Doorbell），
> 让 GCU kernel 能够在不经过 CPU 的情况下直接发起 RDMA 传输。
> 两者底层走的是完全相同的 RC 协议，线路上的数据包格式也完全相同——区别只在于"谁来构建和提交 WQE"。**



我仔细分析了你提供的 DeepEP 代码，现在将对比分析写入 `compare.md`。
已写入 `compare.md`。核心结论：

**DeepEP 不是重新实现了 Verbs API，而是把 `ibv_post_send()` 内部的实现拆开，在 GCU kernel 中自己手动执行。**

具体来说：

1. **协议层完全相同** —— 都是 RC，网线上的数据包一模一样
2. **操作类型不同** —— 本仓库用 Send/Recv（双边），DeepEP 用 RDMA Write（单边）
3. **调用层次不同** —— 本仓库调 `ibv_post_send()` 黑盒 API；DeepEP 拆开这个黑盒，手动做"分配WQE槽位 → 构建WQE写入SQ → 内存屏障 → 敲Doorbell"四步
4. **资源创建都一样** —— DeepEP 的 host 端也要走 `ibv_open_device / ibv_alloc_pd / ibv_reg_mr / ibv_create_qp / modify_qp_to_rts` 这一整套，只是 kernel 看不到而已
5. **绕过 CPU 的原因** —— MoE token 级通信需要微秒级延迟，GCU→CPU→驱动→硬件的往返太慢，所以让 GCU 直接操作硬件