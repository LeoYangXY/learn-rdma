/*
 * ib.c - RDMA InfiniBand 核心操作实现
 *
 * 本文件实现了 RDMA 通信中最关键的三个操作：
 *   1. QP 状态转换 (RESET → INIT → RTR → RTS)
 *   2. 发送数据 (Post Send)
 *   3. 接收数据 (Post SRQ Recv)
 *
 * ============================================================================
 * 【初学者必看: 各种队列的关系】
 * ============================================================================
 *
 * RDMA 的核心抽象就是下面这张图里的几个"队列"。搞清楚它们的关系，
 * 再看代码就会豁然开朗：
 *
 *      ┌───────────────────── QP (Queue Pair, 一对一的"连接") ─────────────────┐
 *      │                                                                      │
 *      │    ┌──────────────────┐              ┌──────────────────┐           │
 *      │    │  SQ (Send Queue) │              │  RQ (Recv Queue) │           │
 *      │    │   发送请求队列    │              │   接收请求队列    │           │
 *      │    │                  │              │                  │           │
 *      │    │  Send WR ①       │              │  Recv WR ①       │           │
 *      │    │  Send WR ②       │              │  Recv WR ②       │           │
 *      │    │  Send WR ③       │              │  ...             │           │
 *      │    └────────┬─────────┘              └────────┬─────────┘           │
 *      │             │ post                            │ post                │
 *      │             │ (用户投递)                       │ (用户投递)           │
 *      └─────────────┼─────────────────────────────────┼─────────────────────┘
 *                    │                                 │
 *                    │ HCA 硬件消费 WR, 产生 WC        │
 *                    ▼                                 ▼
 *             ┌─────────────────────────────────────────────┐
 *             │         CQ (Completion Queue, 完成队列)      │
 *             │   WC①  WC②  WC③ ...                        │
 *             │   用户通过 ibv_poll_cq() 读取             │
 *             └─────────────────────────────────────────────┘
 *
 *  ——— 本项目的特殊之处：使用了 SRQ ———
 *
 *      QP1 ──\
 *      QP2 ───┤→  共享同一个 SRQ (Shared Receive Queue)
 *      QP3 ──/       里面的 Recv WR 被所有 QP 共用
 *
 *      好处：N 个 QP 不用各自预投递一堆 Recv WR，统一从池子里拿，省内存
 *
 *  ——— 数据流向简化记忆 ———
 *
 *      发送端:  用户 → post_send → SQ → HCA → 网络
 *      接收端:  网络 → HCA → 从 SRQ 取 Recv WR → DMA 写入用户缓冲区
 *      双方:    HCA 完成 → 生成 WC → 写入 CQ → 用户 poll 出来
 *
 * ============================================================================
 */

#include <arpa/inet.h>
#include <unistd.h>

#include "ib.h"
#include "debug.h"

/*
 * modify_qp_to_rts - 将 QP 从 RESET 转到 RTS (Ready to Send)
 *
 * QP 状态机路径: RESET → INIT → RTR → RTS
 *   RESET: 刚创建，什么都不能做
 *   INIT:  配置了端口和权限，但不能通信
 *   RTR:   配置了对端路由，可以接收
 *   RTS:   完全就绪，可发送 + 可接收
 */
int modify_qp_to_rts (struct ibv_qp *qp, uint32_t target_qp_num, uint16_t target_lid)
{
    int ret = 0;

    /* =================================================================
     * 第一步: RESET → INIT
     * =================================================================
     * 配置 QP 基本属性:
     *   pkey_index: 分区键索引 (0=默认分区)，用于 IB 子网隔离
     *   port_num:   绑定到 HCA 的哪个物理端口
     *   qp_access_flags: 对端对本 QP 关联内存的访问权限
     *     - LOCAL_WRITE:   允许本地 HCA 写入 (接收数据时必需)
     *     - REMOTE_READ:   允许对端 RDMA Read
     *     - REMOTE_WRITE:  允许对端 RDMA Write
     *     - REMOTE_ATOMIC: 允许对端原子操作 (CAS/Fetch&Add)
     */
    {
	struct ibv_qp_attr qp_attr = {
	    .qp_state        = IBV_QPS_INIT,
	    .pkey_index      = 0,
	    .port_num        = IB_PORT,
	    .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
	                       IBV_ACCESS_REMOTE_READ |
	                       IBV_ACCESS_REMOTE_ATOMIC |
	                       IBV_ACCESS_REMOTE_WRITE,
	};

	/* attr_mask 指定本次修改哪些属性，不在 mask 中的属性不会被修改 */
	ret = ibv_modify_qp (qp, &qp_attr,
			 IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			 IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS);
	check (ret == 0, "Failed to modify qp to INIT.");
    }

    /* =================================================================
     * 第二步: INIT → RTR (Ready to Receive)
     * =================================================================
     * 最关键的一步——配置对端路由信息。完成后 QP 可以接收数据。
     *
     * path_mtu:           路径 MTU，设为端口支持的最大值 4096
     * dest_qp_num:        对端 QP 编号 (通过 TCP 带外交换获得)
     * rq_psn:             接收起始包序号 (PSN)，与对端 sq_psn 一致，这里都设 0
     *                     PSN (Packet Sequence Number) 用于 RC 可靠传输的顺序保证与重传判定
     * max_dest_rd_atomic: 对端可同时向本端发起的 RDMA Read / Atomic 请求数上限
     *                     (本项目只用 Send/Recv，此参数不关键，设 1 足够)
     * min_rnr_timer:      RNR NAK 后对端重试前等待多久。这里值 "12" 是 IB 规范定义的
     *                     查表编码 (0-31 各对应一个时间值)：12 ≈ 0.64ms
     *                     当本端 SRQ 暂时没 Recv WR 时 HCA 会回 RNR NAK，
     *                     对端等此时间后自动重试，配合 rnr_retry=7(无限) 实现背压。
     *
     * AH (Address Handle) 属性 —— IB 路由信息:
     *   is_global=0: 使用 LID 路由 (同子网)
     *   dlid:        对端 LID (类似 IB 子网内的 MAC 地址)
     *   sl:          服务等级 (0=best effort)
     *   port_num:    本地出端口
     */
    {
	struct ibv_qp_attr  qp_attr = {
	    .qp_state           = IBV_QPS_RTR,
	    .path_mtu           = IB_MTU,
	    .dest_qp_num        = target_qp_num,
	    .rq_psn             = 0,
	    .max_dest_rd_atomic = 1,
	    .min_rnr_timer      = 12,
	    .ah_attr.is_global  = 0,
	    .ah_attr.dlid       = target_lid,
	    .ah_attr.sl         = IB_SL,
	    .ah_attr.src_path_bits = 0,
	    .ah_attr.port_num      = IB_PORT,
	};

	ret = ibv_modify_qp(qp, &qp_attr,
			    IBV_QP_STATE | IBV_QP_AV |
			    IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
			    IBV_QP_MIN_RNR_TIMER);
	check (ret == 0, "Failed to change qp to rtr.");
    }

    /* =================================================================
     * 第三步: RTR → RTS (Ready to Send)
     * =================================================================
     * 完成后 QP 完全就绪，可发送 + 可接收。
     *
     * timeout:       本地 ACK 超时 —— 发出数据包后等多久没收到 ACK 就触发重传。
     *                公式: 4.096μs × 2^timeout。值 14 → 约 67 毫秒
     *                (值越大越宽容，越小越激进；一般局域网场景 14 足够)
     * retry_cnt:     传输错误重试次数 (7=最大值，建议默认 7)
     * rnr_retry:     收到 RNR NAK 后的重试次数 (7=无限重试，配合对端的 min_rnr_timer)
     * sq_psn:        发送起始包序号，需与对端 rq_psn 一致，这里都设 0
     * max_rd_atomic: 本端同一时刻可发出的未完成 RDMA Read/Atomic 请求数
     *                (本项目只用 Send/Recv，不关键，设 1 即可)
     */
    {
	struct ibv_qp_attr  qp_attr = {
	    .qp_state      = IBV_QPS_RTS,
	    .timeout       = 14,
	    .retry_cnt     = 7,
	    .rnr_retry     = 7,
	    .sq_psn        = 0,
	    .max_rd_atomic = 1,
	};

	ret = ibv_modify_qp (qp, &qp_attr,
			     IBV_QP_STATE | IBV_QP_TIMEOUT |
			     IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
			     IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
	check (ret == 0, "Failed to modify qp to RTS.");
    }

    return 0;
 error:
    return -1;
}

/*
 * post_send - 向 QP 的发送队列 (SQ) 提交一个 Send with Immediate 请求
 *
 * 【数据发送的完整链路】
 *
 *  用户态                          内核/硬件
 *  ──────                          ────────
 *  1) 填写 SGE (数据在哪)
 *  2) 填写 Send WR (怎么发)
 *  3) ibv_post_send()  ─────→  4) WR 写入 QP 的 SQ (Send Queue)
 *                               5) HCA 硬件通过 DMA 读取 SQ 中的 WR
 *                               6) HCA 根据 SGE 通过 DMA 读取用户缓冲区数据
 *                               7) HCA 将数据封装成 IB 数据包发送到网络
 *                               8) 对端 HCA 收到数据，DMA 写入接收缓冲区
 *                               9) 本端 HCA 在 CQ 中生成 Send WC (发送完成)
 *                              10) 对端 HCA 在 CQ 中生成 Recv WC (接收完成)
 *
 * 【IBV_WR_SEND_WITH_IMM 操作码】
 *   与普通 SEND 相比，额外携带一个 32 位的 imm_data。
 *   接收方可以在 WC 中直接读取 imm_data，无需解析消息体。
 *   本项目用 imm_data 传递发送者的 rank 或控制消息类型。
 *
 * 【IBV_SEND_SIGNALED 标志】
 *   表示此 WR 完成后需要在 CQ 中生成一个 WC。
 *   如果不设此标志，WR 静默完成，不产生 WC（节省 CQ 资源）。
 *   本项目每次发送都设了此标志，生产环境中通常间隔设置以提高性能。
 */
int post_send (uint32_t req_size, uint32_t lkey, uint64_t wr_id,
	       uint32_t imm_data, struct ibv_qp *qp, char *buf)
{
    int ret = 0;
    struct ibv_send_wr *bad_send_wr;  /* 如果提交失败，指向第一个失败的 WR */

    /*
     * SGE (Scatter/Gather Element): 描述数据在内存中的位置
     *   addr:   数据缓冲区的虚拟地址
     *   length: 数据长度 (字节)
     *   lkey:   MR 的本地访问密钥，HCA 用它验证 DMA 访问权限
     *
     * HCA 使用 lkey 查找 MR 表，确认 [addr, addr+length) 在注册的 MR 范围内，
     * 然后将虚拟地址转换为物理地址，执行 DMA 操作。
     */
    struct ibv_sge list = {
	.addr   = (uintptr_t) buf,
	.length = req_size,
	.lkey   = lkey
    };

    /*
     * Send WR (Work Request): 描述一个完整的发送操作
     *   wr_id:      用户自定义标识，WC 中原样返回，用于关联请求与完成
     *   sg_list:    SGE 链表头 (支持多个 SGE 做 gather 发送)
     *   num_sge:    SGE 数量
     *   opcode:     操作类型 (Send with Immediate)
     *   send_flags: 发送标志 (SIGNALED = 完成后产生 WC)
     *   imm_data:   立即数据，需转为网络字节序
     */
    struct ibv_send_wr send_wr = {
	.wr_id      = wr_id,
	.sg_list    = &list,
	.num_sge    = 1,
	.opcode     = IBV_WR_SEND_WITH_IMM,
	.send_flags = IBV_SEND_SIGNALED,
	.imm_data   = htonl (imm_data)     /* 转为网络字节序(大端) */
    };

    /*
     * ibv_post_send: 将 WR 提交到 QP 的发送队列 (SQ)
     * 这是一个异步操作——函数返回时数据不一定已经发送完成。
     * 需要后续通过 ibv_poll_cq 检查 WC 来确认发送是否完成。
     *
     * bad_send_wr: 如果链式提交多个 WR 时部分失败，指向第一个失败的 WR。
     */
    ret = ibv_post_send (qp, &send_wr, &bad_send_wr);
    return ret;
}

/*
 * post_srq_recv - 向共享接收队列 (SRQ) 提交一个接收请求
 *
 * 【接收的完整链路】
 *
 *  1) 用户提前调用 post_srq_recv 投递 Recv WR 到 SRQ
 *     (必须在对端发送之前投递，否则对端会收到 RNR NAK)
 *  2) 对端发送数据到达本端 HCA
 *  3) HCA 从 SRQ 中取出一个可用的 Recv WR
 *  4) HCA 根据 Recv WR 中的 SGE 信息，通过 DMA 将数据写入用户缓冲区
 *  5) HCA 在 CQ 中生成一个 Recv WC，包含:
 *     - wr_id:    用户在投递时设置的标识 (这里设为缓冲区地址)
 *     - imm_data: 对端发送时携带的立即数据
 *     - opcode:   IBV_WC_RECV (表示是接收完成)
 *     - status:   操作状态 (IBV_WC_SUCCESS 表示成功)
 *  6) 用户通过 ibv_poll_cq 获取 WC，处理接收到的数据
 *
 * 【wr_id 的巧妙用法】
 *   本项目将 wr_id 设为接收缓冲区的指针地址 (uint64_t)buf_ptr。
 *   当 WC 返回时，通过 (char *)wc.wr_id 即可直接获得数据所在的缓冲区地址，
 *   无需维护额外的映射表。这是 RDMA 编程中的常见技巧。
 */
int post_srq_recv (uint32_t req_size, uint32_t lkey, uint64_t wr_id, 
		   struct ibv_srq *srq, char *buf)
{
    int ret = 0;
    struct ibv_recv_wr *bad_recv_wr;  /* 如果提交失败，指向第一个失败的 WR */

    /* SGE: 描述接收缓冲区的位置，HCA 会将接收到的数据 DMA 写入此处 */
    struct ibv_sge list = {
	.addr   = (uintptr_t) buf,
	.length = req_size,
	.lkey   = lkey
    };

    /*
     * Recv WR: 描述一个接收操作
     * 注意: Recv WR 没有 opcode 和 send_flags 字段，
     *       因为接收操作很简单——只需指定"数据放哪"即可。
     */
    struct ibv_recv_wr recv_wr = {
	.wr_id   = wr_id,       /* 设为缓冲区地址，方便 WC 中找到数据 */
	.sg_list = &list,
	.num_sge = 1
    };

    /*
     * ibv_post_srq_recv: 将 Recv WR 投递到 SRQ
     * SRQ 中的 WR 被所有关联的 QP 共享。
     * 当任何一个 QP 收到数据，都会从 SRQ 中消费一个 Recv WR。
     */
    ret = ibv_post_srq_recv (srq, &recv_wr, &bad_recv_wr);
    return ret;
}
