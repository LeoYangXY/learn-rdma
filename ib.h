/*
 * ib.h - RDMA InfiniBand 核心头文件
 *
 * ============================================================================
 * 【RDMA 整体通信链路概览】
 * ============================================================================
 *
 * RDMA (Remote Direct Memory Access) 通信的完整链路可以分为以下几个阶段：
 *
 *  阶段1: 资源创建 (setup_ib.c 负责)
 *    1) 打开 IB 设备 (ibv_open_device) → 获得设备上下文 ctx
 *    2) 分配保护域 PD (ibv_alloc_pd) → 隔离不同应用的资源
 *    3) 注册内存区域 MR (ibv_reg_mr) → 将用户态内存注册到网卡，使其可被DMA访问
 *    4) 创建完成队列 CQ (ibv_create_cq) → 用来收割已完成的工作请求(WC)
 *    5) 创建共享接收队列 SRQ (ibv_create_srq) → 多个QP共享同一个接收队列
 *    6) 创建队列对 QP (ibv_create_qp) → 每个连接对应一个QP(包含SQ和RQ)
 *
 *  阶段2: 连接建立 (setup_ib.c + sock.c 负责)
 *    7) 通过 TCP Socket 带外交换双方的 QP 信息 (LID + QP号)
 *    8) 将 QP 状态从 RESET → INIT → RTR → RTS (本文件声明的 modify_qp_to_rts)
 *       - RESET: QP 刚创建时的初始状态
 *       - INIT:  可以开始接收配置，设置端口和访问权限
 *       - RTR (Ready to Receive): 配置了对端信息，可以接收数据
 *       - RTS (Ready to Send): 可以发送数据，此时连接完全就绪
 *
 *  阶段3: 数据传输 (server.c / client.c 负责，调用本文件声明的函数)
 *    9) 提交接收请求 (post_srq_recv) → 往 SRQ 中投递 Recv WR，准备接收数据
 *   10) 提交发送请求 (post_send) → 往 QP 的 SQ 中投递 Send WR，发送数据
 *   11) 轮询完成队列 (ibv_poll_cq) → 从 CQ 中获取 WC，判断操作是否完成
 *   12) 根据 WC 的 opcode 和 imm_data 确定消息类型，做出响应(echo回送)
 *
 *  阶段4: 资源清理 (setup_ib.c 负责)
 *   13) 按照创建的逆序销毁所有资源
 *
 * ============================================================================
 * 【本项目的数据流（Echo模式）】
 * ============================================================================
 *
 *  Client                          Server
 *    |                                |
 *    |  (等待 Server 发 START 信号)    |
 *    |<--- SEND(imm=MSG_CTL_START) ---|  Server预投递Recv后，发START通知Client
 *    |                                |
 *    |--- SEND(imm=client_rank) ----->|  Client开始发送Echo消息
 *    |                                |  Server收到后原样回送(Echo)
 *    |<--- SEND(imm=client_rank) ----|  Server把消息echo回来
 *    |                                |
 *    |  ... (循环 TOT_NUM_OPS 次) ... |
 *    |                                |
 *    |<--- SEND(imm=MSG_CTL_STOP) ---|  Server发送STOP信号
 *    |                                |
 *
 *  每次 SEND 操作都使用 IBV_WR_SEND_WITH_IMM，imm_data 携带发送者的 rank
 *  或控制消息类型(START/STOP)，这样接收方可以知道消息来自哪个peer。
 *
 * ============================================================================
 */

#ifndef IB_H_
#define IB_H_

#include <inttypes.h>
#include <sys/types.h>
#include <endian.h>
#include <byteswap.h>
#include <infiniband/verbs.h>   /* libibverbs 核心头文件，提供所有 RDMA Verbs API */
#include <arpa/inet.h>

/* --------------------------------------------------------------------------
 * 常量定义
 * -------------------------------------------------------------------------- */

/*
 * IB_MTU: InfiniBand 链路层的最大传输单元 (Maximum Transfer Unit)
 * 设为 IBV_MTU_4096 (4096字节)。这是 IB 端口支持的单个数据包最大有效载荷。
 * 如果发送的消息超过 MTU，硬件会自动将其拆分为多个数据包传输。
 */
#define IB_MTU			IBV_MTU_4096

/*
 * IB_PORT: 使用的 IB 物理端口号。
 * IB 网卡(HCA)通常有1-2个物理端口，这里使用端口1。
 */
#define IB_PORT			1

/*
 * IB_SL: Service Level (服务等级)
 * IB 网络支持 16 个服务等级(0-15)，用于 QoS(服务质量)控制。
 * SL=0 是默认的最佳努力(best-effort)服务等级。
 */
#define IB_SL			0

/*
 * IB_WR_ID_STOP: 用作"停止信号"的特殊 Work Request ID。
 * 当 Server 完成所有 Echo 操作后，会用这个 wr_id 发送停止消息。
 * Server 通过检查 WC 中的 wr_id 是否等于此值来判断停止信号是否发送成功。
 * 使用一个大数(高3位为111)来避免与正常的 wr_id (通常是缓冲区地址) 冲突。
 */
#define IB_WR_ID_STOP		0xE000000000000000

/*
 * NUM_WARMING_UP_OPS: 预热操作次数。
 * 前 50万次操作用于预热，不计入性能统计。
 * 目的是让 CPU 缓存、网卡固件等达到稳态，使后续测量更准确。
 */
#define NUM_WARMING_UP_OPS      500000

/*
 * TOT_NUM_OPS: 总操作次数。
 * 共执行 1000万次 Echo 操作。实际用于统计吞吐率的操作次数 = TOT - WARMING_UP。
 */
#define TOT_NUM_OPS             10000000

/*
 * SIG_INTERVAL: 信号间隔 (本项目中未实际使用)。
 * 通常在高性能场景中，不是每个 WR 都设 IBV_SEND_SIGNALED 标志，
 * 而是每隔 SIG_INTERVAL 个 WR 设一次，以减少 CQE 产生的开销。
 * 但本项目的 post_send 中每次都设了 IBV_SEND_SIGNALED。
 */
#define SIG_INTERVAL            1000

/* --------------------------------------------------------------------------
 * 字节序转换工具函数
 * --------------------------------------------------------------------------
 *
 * RDMA 传输中，某些字段(如 imm_data)需要按网络字节序(大端)传输。
 * htonll / ntohll 是 64 位版本的字节序转换函数。
 * (标准库只提供了 16 位 htons/ntohs 和 32 位 htonl/ntohl)
 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll (uint64_t x) {return bswap_64(x); }
static inline uint64_t ntohll (uint64_t x) {return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll (uint64_t x) {return x; }
static inline uint64_t ntohll (uint64_t x) {return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

/* --------------------------------------------------------------------------
 * QPInfo: 用于带外 (out-of-band) 交换的 QP 连接信息
 * --------------------------------------------------------------------------
 *
 * 【为什么需要带外交换？】
 * RDMA 连接建立时，双方需要知道对方的以下信息才能将 QP 转为 RTR/RTS 状态：
 *   - lid:    本地标识符 (Local Identifier)，类似于 IB 子网内的"MAC地址"
 *   - qp_num: QP 编号，每个 QP 在 HCA 上有唯一编号
 *   - rank:   节点逻辑编号（本项目自定义），用于确定 QP 的对应关系
 *
 * 这些信息无法通过 RDMA 本身传递（因为 RDMA 连接还没建立），
 * 所以需要通过 TCP Socket（带外通道）来交换。
 *
 * packed 属性确保结构体在网络传输时没有填充字节，双方内存布局一致。
 */
struct QPInfo {
    uint16_t lid;       /* 本地标识符 (LID)，IB 子网交换机用它来路由数据包 */
    uint32_t qp_num;    /* QP 编号，HCA 内唯一标识一个 QP */
    uint32_t rank;      /* 节点的逻辑编号，用于多对多连接时匹配正确的 QP 对 */
}__attribute__ ((packed));

/* --------------------------------------------------------------------------
 * MsgType: 控制消息类型，通过 SEND_WITH_IMM 的 imm_data 字段传递
 * --------------------------------------------------------------------------
 *
 * imm_data (Immediate Data) 是 IB Send with Immediate 操作中的一个32位附加数据。
 * 它的特点是：接收方无需查看消息内容即可读取该值（直接在 WC 中），
 * 非常适合用来传递轻量级的控制信息。
 *
 * 本项目中：
 *   - 正常 Echo 消息: imm_data = 发送者的 rank (如 0, 1, 2...)
 *   - 控制消息: imm_data = MSG_CTL_START (100) 或 MSG_CTL_STOP (101)
 *
 * 使用 100+ 的值作为控制消息类型，与正常的 rank 值(通常很小)区分开。
 */
enum MsgType {
    MSG_CTL_START = 100,    /* Server → Client: "我已准备好接收，你可以开始发送了" */
    MSG_CTL_STOP,           /* Server → Client: "测试完成，停止发送" (值=101) */
};

/* --------------------------------------------------------------------------
 * 函数声明
 * -------------------------------------------------------------------------- */

/*
 * modify_qp_to_rts: 将 QP 状态从 RESET 一路转换到 RTS (Ready to Send)
 *
 * 【QP 状态机转换路径】
 *   RESET → INIT → RTR (Ready to Receive) → RTS (Ready to Send)
 *
 * 每个状态转换需要提供不同的参数：
 *   RESET → INIT: 端口号、访问权限
 *   INIT  → RTR:  对端 QP 信息(LID、QP号)、路径 MTU、PSN 等
 *   RTR   → RTS:  超时、重试次数、PSN 等
 *
 * 参数:
 *   qp         - 要修改状态的本地 QP
 *   qp_num     - 对端 QP 的编号 (用于 RTR 阶段配置路由)
 *   lid        - 对端的 LID (用于 RTR 阶段配置路由)
 */
int modify_qp_to_rts (struct ibv_qp *qp, uint32_t qp_num, uint16_t lid);

/*
 * 【lkey vs rkey: 初学者重点区分】
 *
 *   ibv_reg_mr 注册成功后会返回两把"钥匙"，分别是 lkey 和 rkey：
 *
 *     lkey (Local Key)  - 本端 HCA 自己做 DMA 时用的本地凭证
 *                         出现在本端 post_send / post_recv 的 SGE 里
 *                         永远只在本端使用，不传给对端
 *
 *     rkey (Remote Key) - 对端做 RDMA Read / RDMA Write 时用的远程凭证
 *                         必须通过带外通道(如 TCP)把 rkey + addr 告知对端
 *                         对端在 Send WR 的 wr.rdma.rkey 字段里携带
 *
 *   本项目只用了 Send/Recv (双边操作)，所以只用到 lkey，不需要 rkey。
 *   真正的"RDMA 单边操作"(Read/Write) 才需要 rkey。
 *
 * --------------------------------------------------------------------------
 *
 * post_send: 向 QP 的发送队列 (SQ) 提交一个 Send with Immediate 请求
 *
 * 【发送流程】
 *   1) 构造 SGE (Scatter/Gather Element): 描述要发送的数据在内存中的位置
 *   2) 构造 Send WR (Work Request): 包含 SGE + 操作码 + 标志 + imm_data
 *   3) 调用 ibv_post_send 将 WR 提交到 QP 的 SQ 中
 *   4) HCA 硬件异步处理该 WR，完成后在 CQ 中生成一个 WC (Work Completion)
 *
 * 参数:
 *   req_size   - 要发送的数据大小(字节)
 *   lkey       - 内存区域的本地访问密钥(来自 MR 注册), HCA 用它验证 DMA 权限
 *   wr_id      - 用户自定义的 WR 标识，完成时可在 WC 中读回(用于关联请求和完成)
 *   imm_data   - 立即数据(32位)，接收方可在 WC 中直接获取，无需读消息体
 *   qp         - 要提交请求的 QP
 *   buf        - 发送数据缓冲区的起始地址 (必须在 MR 注册范围内)
 */
int post_send (uint32_t req_size, uint32_t lkey, uint64_t wr_id, 
	       uint32_t imm_data, struct ibv_qp *qp, char *buf);

/*
 * post_srq_recv: 向共享接收队列 (SRQ) 提交一个接收请求
 *
 * 【接收流程】
 *   1) 构造 SGE: 描述接收缓冲区的位置(数据到达后 HCA 会 DMA 写入此处)
 *   2) 构造 Recv WR: 包含 SGE 信息
 *   3) 调用 ibv_post_srq_recv 将 WR 提交到 SRQ 中
 *   4) 当对端发来数据时，HCA 从 SRQ 中消费一个 Recv WR，
 *      将数据 DMA 写入对应的缓冲区，然后在 CQ 中生成一个 WC
 *
 * 【为什么用 SRQ 而不是 RQ？】
 *   普通模式: 每个 QP 有自己的 RQ，N 个 QP 需要分别预投递 Recv WR
 *   SRQ 模式: 所有 QP 共享一个 SRQ，Recv WR 集中管理，更高效
 *   好处: 减少总的 Recv WR 数量，降低内存开销，简化管理
 *
 * 参数:
 *   req_size   - 接收缓冲区大小(字节)
 *   lkey       - 内存区域的本地访问密钥
 *   wr_id      - 用户自定义标识，通常设为缓冲区地址，方便在 WC 中找到数据
 *   srq        - 要提交请求的 SRQ
 *   buf        - 接收缓冲区的起始地址
 */
int post_srq_recv (uint32_t req_size, uint32_t lkey, uint64_t wr_id, 
		   struct ibv_srq *srq, char *buf);


#endif /*ib.h*/
