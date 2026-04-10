/*
 * setup_ib.h - RDMA 资源管理头文件
 *
 * 定义了 IBRes 结构体，集中管理所有 RDMA 资源。
 * 这些资源的创建顺序很重要，因为存在依赖关系：
 *
 * 【RDMA 资源依赖关系图】
 *
 *  ibv_device (IB 设备)
 *      │
 *      └──→ ctx (设备上下文) ──────────────────────────────────┐
 *            │                                                 │
 *            ├──→ pd (保护域) ─────────────────┐               │
 *            │      │                          │               │
 *            │      ├──→ mr (内存区域)         │               │
 *            │      │     └── ib_buf (数据缓冲区)              │
 *            │      │                          │               │
 *            │      ├──→ srq (共享接收队列)    │               │
 *            │      │                          │               │
 *            │      └──→ qp[] (队列对数组) ────┘               │
 *            │             │                                   │
 *            │             └── 关联 cq + srq                   │
 *            │                                                 │
 *            ├──→ cq (完成队列) ───────────────────────────────┘
 *            │
 *            ├──→ port_attr (端口属性，含 LID)
 *            └──→ dev_attr  (设备属性，含最大队列深度等)
 *
 * 销毁时按照创建的逆序：qp → srq → cq → mr → pd → ctx → buf
 */

#ifndef SETUP_IB_H_
#define SETUP_IB_H_

#include <infiniband/verbs.h>

/*
 * IBRes: 集中管理所有 RDMA 资源的结构体
 *
 * 一个完整的 RDMA 连接需要以下资源，它们各自的作用如下：
 */
struct IBRes {
    struct ibv_context      *ctx;       /* 设备上下文: 代表打开的 IB 设备，所有操作的根基 */
    struct ibv_pd           *pd;        /* 保护域 (Protection Domain): 资源隔离的安全边界，
                                         * 只有同一个 PD 下的 QP 和 MR 才能配合使用 */
    struct ibv_mr           *mr;        /* 内存区域 (Memory Region): 已注册到 HCA 的内存，
                                         * HCA 可以直接 DMA 访问。注册时获得 lkey/rkey */
    struct ibv_cq           *cq;        /* 完成队列 (Completion Queue): 收割已完成操作的地方，
                                         * 每个完成的 Send/Recv 都会在这里产生一个 WC */
    struct ibv_qp          **qp;        /* 队列对数组 (Queue Pair): 每个连接一个 QP，
                                         * QP = SQ(发送队列) + RQ(接收队列)。
                                         * 本项目 RQ 由 SRQ 替代 */
    struct ibv_srq          *srq;       /* 共享接收队列 (Shared Receive Queue):
                                         * 所有 QP 共享的接收队列，节省 Recv WR 资源 */
    struct ibv_port_attr     port_attr; /* 端口属性: 包含 LID、端口状态、最大 MTU 等 */
    struct ibv_device_attr   dev_attr;  /* 设备属性: 包含最大 CQE 数、最大 QP 数等硬件能力 */

    int     num_qps;       /* QP 数量: Server端 = 客户端数量, Client端 = 服务器数量 */
    char   *ib_buf;        /* RDMA 数据缓冲区指针，注册为 MR 的内存区域 */
    size_t  ib_buf_size;   /* 缓冲区大小 = msg_size × num_concurr_msgs × num_qps */
};

/* 全局 IB 资源实例，整个程序共享 */
extern struct IBRes ib_res;

/*
 * setup_ib: 创建所有 RDMA 资源并建立连接
 * 这是 RDMA 初始化的核心函数，执行完后所有 QP 都处于 RTS 状态，可以通信。
 */
int  setup_ib ();

/*
 * close_ib_connection: 按逆序销毁所有 RDMA 资源
 * 顺序: QP → SRQ → CQ → MR → PD → ctx → buf
 */
void close_ib_connection ();

/*
 * connect_qp_server / connect_qp_client:
 * 通过 TCP Socket 交换 QP 信息，然后将 QP 转为 RTS 状态
 * Server 端监听等待 Client 连接，Client 端主动连接 Server
 */
int  connect_qp_server ();
int  connect_qp_client ();

#endif /*setup_ib.h*/
