/*
 * rdma_verbs.c - RDMA 高频数据路径：QP 状态转换 + 收发操作
 *
 * ============================================================================
 * 【背景知识】
 *
 * 1. 什么是驱动？
 *    驱动 = 让操作系统能跟硬件对话的一段代码。
 *    硬件只认识"往寄存器写某个值"这种低级操作。
 *    驱动把这些翻译成程序能用的接口（如 send()、recv()）。
 *
 * 2. 普通网卡怎么工作？（TCP，每次都经过内核）
 *    程序 → send()系统调用 → 切到内核 → 内核驱动操作网卡 → 切回用户态
 *    问题：每次发包都要穿越内核，开销大。
 *
 * 3. RDMA 网卡怎么工作？（初始化走内核驱动，运行时走用户态驱动）
 *    a) 初始化时（一次性，经过内核）:
 *       ibv_open_device() → 内核驱动初始化硬件，
 *       把网卡的硬件寄存器通过 mmap 映射到你的进程地址空间。
 *
 *    b) 数据传输时（高频操作，不经过内核）:
 *       ibv_post_send() → 往 mmap 映射的内存写东西
 *                        → 这块内存直接对应网卡硬件寄存器
 *                        → 写内存 = 操纵网卡 = 网卡立刻干活
 *       ibv_poll_cq()   → 读 mmap 映射的内存 → 拿到完成通知
 *
 * 4. 为什么叫"用户态驱动"？
 *    驱动的本质 = 操纵硬件寄存器。
 *    传统做法：只有内核能操纵寄存器（安全但慢）。
 *    RDMA 做法：通过 mmap 把寄存器映射到用户进程，
 *    用户程序直接写内存 = 写寄存器 = 操纵网卡。
 *    驱动逻辑跑在用户态，所以叫"用户态驱动"。
 *
 * 5. 总结
 *    libibverbs（ibv_xxx 函数所在的库）= 用户态驱动。
 *    内核驱动负责：初始化 + 权限管理 + mmap 映射。
 *    用户态驱动负责：高速数据路径（post_send / poll_cq）。
 *
 * ============================================================================
 * 【本文件做的事情】
 *
 *  modify_qp_to_rts()  QP: RESET → INIT → RTR → RTS
 *  post_send()         往 SQ 投递一个发送请求——走用户态驱动，不经过内核
 *  post_srq_recv()     往 SRQ 投递一个接收请求——走用户态驱动，不经过内核
 * ============================================================================
 */

#include "common.h"

/*
 * modify_qp_to_rts - QP 状态机: RESET → INIT → RTR → RTS
 *
 * 【QP 状态机】
 *
 *        配置端口+权限        填入对端地址         配置超时+重试
 *  RESET ─────────────→ INIT ─────────────→ RTR ─────────────→ RTS
 *             动作1              动作2              动作3
 *
 *  每次调 ibv_modify_qp() = 执行一个动作，触发一次状态转变。
 *  动作 → 状态变化 → 解锁新能力：
 *
 *    状态    能做什么
 *    ────    ────────
 *    RESET   什么都不能
 *    INIT    可以被配置，但不能收发
 *    RTR     可以接收（网卡已知道对端是谁）
 *    RTS     可以接收 + 发送（完全就绪）
 *
 *  为什么不能一步到位？
 *  网卡硬件要求分步提供不同层面的信息，每步校验通过才进入下一状态。
 *
 * 【类比 TCP】
 *    RESET → INIT  ≈  socket() + bind()
 *    INIT  → RTR   ≈  知道了对方 IP:PORT
 *    RTR   → RTS   ≈  connect() 完成，可以 send/recv 了
 */
int modify_qp_to_rts(struct ibv_qp *qp, uint32_t target_qp_number,
                     uint16_t target_lid, union ibv_gid *dgid)
{
    int ret = 0;

    /* ===== RESET → INIT: 配置端口 + 访问权限 =====
     *
     * 【物理端口与 QP 的关系】
     *
     *  RDMA 网卡有 1~2 个物理端口（即网卡背面的网线接口）。
     *  每个物理端口连着一根网线，通往交换机或直连对端。
     *  port_num 就是指定"这个 QP 的数据从哪个端口进出"。
     *
     *  一个物理端口上可以创建几十万个 QP，每个 QP 是一条独立连接。
     *  到了 QP 这一层，就跟 TCP 靠 65536 个逻辑 PORT 区分不同连接一样了：
     *
     *    TCP:   一个物理网口 → 65536 个逻辑 PORT → 区分不同连接
     *    RDMA:  一个物理端口 → 几十万个 QP       → 区分不同连接
     */
    {
        struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_INIT,       /* 目标状态: INIT */
            .pkey_index      = 0,                  /* 分区键索引（用默认 0，类似 VLAN ID） */
            .port_num        = IB_PORT,            /* 绑定到哪个物理端口（我们用端口 1） */
            .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |    /* 允许本地写入（接收数据时必须） */
                               IBV_ACCESS_REMOTE_READ |    /* 允许对端 RDMA Read 我的内存 */
                               IBV_ACCESS_REMOTE_ATOMIC |  /* 允许对端对我做原子操作 */
                               IBV_ACCESS_REMOTE_WRITE,    /* 允许对端 RDMA Write 我的内存 */
        };
        ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                            IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
        check(ret == 0, "RESET→INIT failed");
    }

    /* ===== INIT → RTR: 配置对端路由（最关键的一步） =====
     *
     * 其实别的都是模板代码，最关键的就是 dgid 和 dest_qp_number：
     * 用于告诉网卡"对端在哪台机器（GID）、对端的哪个 QP（qp_number）"。
     */
    {
        struct ibv_qp_attr attr = {
            .qp_state           = IBV_QPS_RTR,         /* 目标状态: Ready to Receive */
            .path_mtu           = IB_MTU,              /* 单个包最大载荷 = 1024 字节（受底层网口 MTU 限制） */
            .dest_qp_num        = target_qp_number,    /* ★ 对端 QP 编号（来自交换的 QPInfo，对端网卡靠它找到 QP） */
            .rq_psn             = 0,                   /* 接收包序号起点（类似 TCP 序列号，双方约定都从 0 开始） */
            .max_dest_rd_atomic = 1,                   /* 对端能同时发几个 RDMA Read（本项目不用，填 1） */
            .min_rnr_timer      = 12,                  /* SRQ 空时告诉对端等 ≈0.6ms 再重试 */
            .ah_attr.is_global     = 1,                /* ★ RoCE 必须设 1：使用 GID 全局路由 */
            .ah_attr.dlid          = target_lid,       /* 对端 LID（RoCE 下为 0，没人看） */
            .ah_attr.sl            = IB_SL,            /* 服务等级 = 0（best effort，类似网络 QoS 优先级） */
            .ah_attr.src_path_bits = 0,                /* LID 掩码（RoCE 不用，填 0） */
            .ah_attr.port_num      = IB_PORT,          /* 从哪个物理端口发出去 */
        };
        /* GRH (Global Routing Header) —— RoCE 的核心路由信息 */
        attr.ah_attr.grh.dgid       = *dgid;          /* ★ 对端 GID（= 对端 IP 的 128-bit 表示）
                                                        *   网卡拿到后: GID → 提取 IP → ARP 查 MAC → 发包 */
        attr.ah_attr.grh.sgid_index = 1;              /* 本地 GID 表第 1 项（GID[1] = IPv4 mapped 地址） */
        attr.ah_attr.grh.hop_limit  = 64;             /* TTL：最多经过 64 个路由器（防止环路） */

        ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_AV |
                            IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                            IBV_QP_MIN_RNR_TIMER);
        check(ret == 0, "INIT→RTR failed");
    }

    /* ===== RTR → RTS: 配置超时和重试，完成后可发送 =====
     *
     * 核心就是配置"出错了怎么办"——超时重传策略。
     * 真正重要的：timeout（多久没 ACK 就重传）和 retry_cnt（最多重传几次）。
     * 其他都是模板值。
     */
    {
        struct ibv_qp_attr attr = {
            .qp_state      = IBV_QPS_RTS,  /* 目标状态: Ready to Send（完全就绪） */
            .timeout       = 14,       /* ACK 超时: 4.096μs × 2^14 ≈ 67ms（超时就重传） */
            .retry_cnt     = 7,        /* 传输错误最多重试 7 次（7 = 最大值） */
            .rnr_retry     = 7,        /* 对端 SRQ 空时无限重试（7 = 无限） */
            .sq_psn        = 0,        /* 发送包序号起点（必须与对端 rq_psn 一致） */
            .max_rd_atomic = 1,        /* 我能同时发几个 RDMA Read（本项目不用，填 1） */
        };
        ret = ibv_modify_qp(qp, &attr,
                            IBV_QP_STATE | IBV_QP_TIMEOUT |
                            IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                            IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
        check(ret == 0, "RTR→RTS failed");
    }

    return 0;
 error:
    return -1;
}

/*
 * 【WR 与 WQE 的区别】
 *   WR (Work Request)  — 软件/用户态层面，用户直接构造的请求描述（ibv_send_wr 结构体）。
 *                         调用 post 时传入，调用返回后即可释放。
 *   WQE (Work Queue Element) — 硬件/队列内部层面，驱动将 WR 翻译后写入 SQ 的条目。
 *                         格式由厂商硬件定义，用户不可见，驻留到硬件消费完毕为止。
 *
 *   流程：ibv_post_send(wr) → 驱动把 WR 翻译成 WQE 写入 SQ → 网卡取出 WQE 执行 → 产生 CQE 进入 CQ
 *   类比：WR 是你填的"快递下单表"，WQE 是快递公司内部系统生成的"调度工单"。
*/


/*
 * post_send - 往 QP 的 SQ 投递一个 Send with Immediate 请求
 *
 * 【参数逐个解释】
 *   req_size  — 要发送的字节数。填入 SGE.length，网卡从 buf 起始读这么多字节。
 *   lkey      — 本地内存区域的访问密钥（ibv_reg_mr 注册时返回的 mr->lkey）。
 *               网卡 DMA 读取用户内存时必须验证此密钥，防止越权访问。
 *   wr_id     — 用户自定义的 64 位标识，原样回传到 WC.wr_id。
 *               用途：poll CQ 拿到完成通知后，靠它定位是哪个请求完成了。
 *               搬运链路：
 *                 用户填 wr.wr_id
 *                   → 驱动把 WR 翻译成 WQE，wr_id 写进 WQE 字段
 *                   → WQE 进入 SQ
 *                   → 网卡执行操作后，生成 CQE，把 WQE 里的 wr_id 原样拷进 CQE
 *                   → CQE 进入 CQ
 *                   → ibv_poll_cq() 把 CQE 翻译成 ibv_wc，wr_id 即当初填的值
 *               全程没人解释这个值的含义，纯粹原样搬运，所以可以随便填：
 *               数组下标、缓冲区指针、自编 ID 都行——对硬件就是 64 位不透明数据。
 *   imm_data  — 32 位立即数，跟数据包一起飞到对端，对端在 WC.imm_data 里直接读取。
 *               生效条件：opcode 必须是 IBV_WR_SEND_WITH_IMM 或 IBV_WR_RDMA_WRITE_WITH_IMM。
 *               硬件行为：
 *                 发送方网卡把这 32 位封进 IB 包头 → 对端网卡收到后塞进对端 WC.imm_data，
 *                 并在 wc.wc_flags 里打上 IBV_WC_WITH_IMM 标志。
 *               用途：给对端捎带 32 位元信息（消息长度、序号、类型标记等），
 *               对端不用读消息体（buf）就能拿到。
 *               【关键】这 32 位的语义完全由收发双方自行约定：
 *                 编码规则（哪几位放什么、怎么拆字段）、解码方式都是协议设计者的事，
 *                 硬件只负责把这 32 位原样从发送端搬到接收端，不解释里面的内容。
 *               【为什么要 htonl】
 *                 这 32 位会上网，两端 CPU 字节序可能不同（x86/ARM 小端，部分架构大端）。
 *                 网络协议（含 IB）规定上网一律用大端：
 *                   发送前 htonl(x)：主机序 → 网络序（大端）
 *                   接收后 ntohl(x)：网络序 → 主机序
 *                 小端机上 htonl 是字节翻转，大端机上是空操作，无脑套用即可。
 *                 对比：wr_id 不上网（只在本机内存搬运），所以不需要字节序转换。
 *   qp        — 目标 Queue Pair，决定消息走哪条连接发出去。
 *   buf       — 发送缓冲区起始地址。网卡从这里 DMA 读数据上网。
 *               必须落在 lkey 对应的 MR 注册范围内，否则网卡报错。
 *
 * 三者组合关系：buf + req_size + lkey → 构成一个 SGE（Scatter-Gather Element），
 * 告诉网卡"从哪读、读多少、用什么钥匙验证权限"。
 *
 * 这是异步操作：函数返回 ≠ 数据已发完。要 poll CQ 拿到 WC 才算真完成。
 */
int post_send(uint32_t req_size, uint32_t lkey, uint64_t wr_id,
              uint32_t imm_data, struct ibv_qp *qp, char *buf)
{
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {
        .addr   = (uintptr_t)buf,
        .length = req_size,
        .lkey   = lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = wr_id,
        .sg_list    = &sge,
        .num_sge    = 1,                    /* 几个 SGE。可多个，网卡 gather 拼成一条消息。本项目数据连续，1 就够 */
        .opcode     = IBV_WR_SEND_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data   = htonl(imm_data),     /* 上网就转大端：对端 ntohl(wc.imm_data) 拿回原值；语义由双方约定 */
    };
    return ibv_post_send(qp, &wr, &bad_wr);
}

/*
 * post_srq_recv - 往 SRQ 投递一个接收请求（预占坑）
 *
 * 【为什么要提前投递？】
 *   RDMA 接收是被动的：对端发包来时，网卡从 SRQ 取一个 Recv WR，
 *   把数据 DMA 写入 WR 指定的缓冲区。如果 SRQ 空了 → RNR NAK（拒收）。
 *
 * 【wr_id 技巧】
 *   设为缓冲区地址。WC 返回时 (char*)wc.wr_id 就是数据所在位置。
 */
int post_srq_recv(uint32_t req_size, uint32_t lkey, uint64_t wr_id,
                  struct ibv_srq *srq, char *buf)
{
    struct ibv_recv_wr *bad_wr;
    struct ibv_sge sge = {
        .addr   = (uintptr_t)buf,
        .length = req_size,
        .lkey   = lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id   = wr_id,
        .sg_list = &sge,
        .num_sge = 1,
    };
    return ibv_post_srq_recv(srq, &wr, &bad_wr);
}
