/*
 * setup_ib.c - RDMA 资源创建、连接建立与清理
 *
 * ============================================================================
 * 【本文件在 RDMA 通信链路中的位置】
 * ============================================================================
 *
 * 本文件负责 RDMA 通信的"阶段1: 资源创建"和"阶段2: 连接建立"。
 * 它是整个 RDMA 程序的基础设施层，所有数据传输(server.c/client.c)都依赖
 * 这里创建的资源。
 *
 * 核心函数:
 *   setup_ib()           - 创建所有 RDMA 资源 + 建立连接
 *   connect_qp_server()  - Server端: 通过TCP交换QP信息 + QP状态转换
 *   connect_qp_client()  - Client端: 通过TCP交换QP信息 + QP状态转换
 *   close_ib_connection()- 按逆序销毁所有资源
 *
 * ============================================================================
 */

#include <arpa/inet.h>
#include <unistd.h>
#include <malloc.h>

#include "sock.h"
#include "ib.h"
#include "debug.h"
#include "config.h"
#include "setup_ib.h"

/* 全局 IB 资源实例 */
struct IBRes ib_res;

/*
 * connect_qp_server - Server 端的 QP 连接建立流程
 *
 * 【Server 端连接建立的完整步骤】
 *
 *  1) 创建 TCP 服务端 Socket，监听端口
 *  2) Accept 所有 Client 的 TCP 连接
 *  3) 准备本地 QP 信息 (LID + QP号 + rank)
 *  4) 通过 TCP 接收每个 Client 的 QP 信息
 *  5) 通过 TCP 发送本地 QP 信息给每个 Client
 *  6) 使用对端 QP 信息将本地 QP 转为 RTS 状态
 *  7) 通过 TCP 与 Client 做同步确认 (确保双方都已完成 QP 状态转换)
 *  8) 关闭 TCP 连接 (后续通信全部走 RDMA，不再需要 TCP)
 *
 * 【为什么需要 TCP 辅助？】
 *  RDMA 连接建立是一个"先有鸡还是先有蛋"的问题：
 *  要建立 RDMA 连接，需要知道对端的 QP 信息；
 *  但在 RDMA 连接建立之前，无法通过 RDMA 传输这些信息。
 *  所以需要一个"带外通道"(out-of-band channel)，即 TCP Socket。
 *
 * 【rank 的作用】
 *  在多对多连接中 (N 个 Client 连接 M 个 Server)，TCP accept 的顺序
 *  可能与 Client 的逻辑编号(rank)不一致。
 *  所以需要在 QPInfo 中携带 rank，根据 rank 匹配正确的 QP 对。
 */
int connect_qp_server ()
{
    int		     ret	= 0, n = 0, i = 0;
    int              num_peers  = config_info.num_clients;  /* Server 的对端数 = Client 数 */
    int		     sockfd	= 0;
    int		    *peer_sockfd= NULL;       /* 每个 Client 对应一个 TCP socket */
    struct sockaddr_in	 peer_addr;
    socklen_t		 peer_addr_len	= sizeof(struct sockaddr_in);
    char sock_buf[64]			= {'\0'};
    struct QPInfo   *local_qp_info  = NULL;   /* 本端 QP 信息数组 */
    struct QPInfo   *remote_qp_info = NULL;   /* 对端 QP 信息数组 */

    /* 步骤1: 创建 TCP 服务端 Socket 并绑定端口 */
    sockfd = sock_create_bind(config_info.sock_port);
    check(sockfd > 0, "Failed to create server socket.");
    listen(sockfd, 5);

    /* 步骤2: 为每个 Client 分配一个 socket fd，然后 accept 连接 */
    peer_sockfd = (int *) calloc (num_peers, sizeof(int));
    check (peer_sockfd != NULL, "Failed to allocate peer_sockfd");

    for (i = 0; i < num_peers; i++) {
	peer_sockfd[i] = accept(sockfd, (struct sockaddr *)&peer_addr,
				&peer_addr_len);
	check (peer_sockfd[i] > 0, "Failed to create peer_sockfd[%d]", i);
    }

    /* 步骤3: 准备本地 QP 信息
     * 每个 QP 对应一个 Client 连接，信息包括:
     *   lid:     本端口的 LID (从 port_attr 获取)
     *   qp_num:  QP 编号 (创建 QP 时由 HCA 分配)
     *   rank:    本节点的逻辑编号 */
    local_qp_info = (struct QPInfo *) calloc (num_peers, 
					      sizeof(struct QPInfo));
    check (local_qp_info != NULL, "Failed to allocate local_qp_info");

    for (i = 0; i < num_peers; i++) {
	local_qp_info[i].lid	= ib_res.port_attr.lid; 
	local_qp_info[i].qp_num = ib_res.qp[i]->qp_num;
	local_qp_info[i].rank   = config_info.rank;
    }

    /* 步骤4: 通过 TCP 接收每个 Client 的 QP 信息
     * sock_get_qp_info 内部做了网络字节序到主机字节序的转换 */
    remote_qp_info = (struct QPInfo *) calloc (num_peers, 
					       sizeof(struct QPInfo));
    check (remote_qp_info != NULL, "Failed to allocate remote_qp_info");

    for (i = 0; i < num_peers; i++) {
	ret = sock_get_qp_info (peer_sockfd[i], &remote_qp_info[i]);
	check (ret == 0, "Failed to get qp_info from client[%d]", i);
    }
    
    /* 步骤5: 通过 TCP 发送本地 QP 信息给每个 Client
     * 
     * 【关键: rank 匹配】
     * TCP accept 的顺序可能与 Client rank 不一致。
     * 例如: peer_sockfd[0] 可能连接的是 rank=1 的 Client。
     * 所以这里根据 remote_qp_info[j].rank 找到正确的 QP 索引 (peer_ind)，
     * 将对应的 local_qp_info 发给正确的 Client。 */
    int peer_ind = -1;
    int j        = 0;
    for (i = 0; i < num_peers; i++) {
	peer_ind = -1;
	for (j = 0; j < num_peers; j++) {
	    if (remote_qp_info[j].rank == i) {
		peer_ind = j;
		break;
	    }
	}
	ret = sock_set_qp_info (peer_sockfd[i], &local_qp_info[peer_ind]);
	check (ret == 0, "Failed to send qp_info to client[%d]", peer_ind);
    }

    /* 步骤6: 使用对端信息将所有 QP 转为 RTS 状态
     * modify_qp_to_rts 内部完成: RESET → INIT → RTR → RTS 
     * 需要对端的 qp_num 和 lid 作为 RTR 阶段的路由参数 */
    log (LOG_SUB_HEADER, "Start of IB Config");
    for (i = 0; i < num_peers; i++) {
	peer_ind = -1;
	for (j = 0; j < num_peers; j++) {
	    if (remote_qp_info[j].rank == i) {
		peer_ind = j;
		break;
	    }
	}
	ret = modify_qp_to_rts (ib_res.qp[peer_ind], 
				remote_qp_info[i].qp_num, 
				remote_qp_info[i].lid);
	check (ret == 0, "Failed to modify qp[%d] to rts", peer_ind);

	log ("\tqp[%"PRIu32"] <-> qp[%"PRIu32"]", 
	     ib_res.qp[peer_ind]->qp_num, remote_qp_info[i].qp_num);
    }
    log (LOG_SUB_HEADER, "End of IB Config");

    /* 步骤7: 通过 TCP 与 Client 做同步确认
     * 确保双方都已完成 QP 状态转换后再开始 RDMA 通信。
     * 如果一端的 QP 还没到 RTS 就开始发数据，会导致通信失败。
     *
     * 同步协议: Server 先读 (等 Client 写)，然后 Server 写 (通知 Client) */
    for (i = 0; i < num_peers; i++) {
	n = sock_read (peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
	check (n == sizeof(SOCK_SYNC_MSG), "Failed to receive sync from client");
    }
    
    for (i = 0; i < num_peers; i++) {
	n = sock_write (peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
	check (n == sizeof(SOCK_SYNC_MSG), "Failed to write sync to client");
    }

    /* 步骤8: 关闭所有 TCP 连接
     * 后续所有通信都通过 RDMA 进行，TCP 使命完成 */
    for (i = 0; i < num_peers; i++) {
	close (peer_sockfd[i]);
    }
    free (peer_sockfd);
    close (sockfd);
    
    return 0;

 error:
    if (peer_sockfd != NULL) {
	for (i = 0; i < num_peers; i++) {
	    if (peer_sockfd[i] > 0) {
		close (peer_sockfd[i]);
	    }
	}
	free (peer_sockfd);
    }
    if (sockfd > 0) {
	close (sockfd);
    }
    
    return -1;
}

/*
 * connect_qp_client - Client 端的 QP 连接建立流程
 *
 * 与 Server 端对称，但方向相反:
 *   1) 主动 TCP 连接到每个 Server
 *   2) 先发送本地 QP 信息给 Server
 *   3) 再接收 Server 的 QP 信息
 *   4) 将 QP 转为 RTS 状态
 *   5) 同步确认 (Client 先写，再读)
 */
int connect_qp_client ()
{
    int ret	       = 0, n = 0, i = 0;
    int num_peers      = ib_res.num_qps;     /* Client 的对端数 = Server 数 */
    int *peer_sockfd   = NULL;
    char sock_buf[64]  = {'\0'};

    struct QPInfo *local_qp_info  = NULL;
    struct QPInfo *remote_qp_info = NULL;

    /* 步骤1: 主动连接到每个 Server 的 TCP 端口 */
    peer_sockfd = (int *) calloc (num_peers, sizeof(int));
    check (peer_sockfd != NULL, "Failed to allocate peer_sockfd");

    for (i = 0; i < num_peers; i++) {
	peer_sockfd[i] = sock_create_connect (config_info.servers[i],
					      config_info.sock_port);
	check (peer_sockfd[i] > 0, "Failed to create peer_sockfd[%d]", i);
    }

    /* 步骤2: 准备本地 QP 信息 */
    local_qp_info = (struct QPInfo *) calloc (num_peers, 
					      sizeof(struct QPInfo));
    check (local_qp_info != NULL, "Failed to allocate local_qp_info");

    for (i = 0; i < num_peers; i++) {
	local_qp_info[i].lid     = ib_res.port_attr.lid; 
	local_qp_info[i].qp_num  = ib_res.qp[i]->qp_num; 
	local_qp_info[i].rank    = config_info.rank;
    }

    /* 步骤3: 通过 TCP 发送本地 QP 信息给 Server */
    for (i = 0; i < num_peers; i++) {
	ret = sock_set_qp_info (peer_sockfd[i], &local_qp_info[i]);
	check (ret == 0, "Failed to send qp_info[%d] to server", i);
    }

    /* 步骤4: 通过 TCP 接收 Server 的 QP 信息 */
    remote_qp_info = (struct QPInfo *) calloc (num_peers, 
					       sizeof(struct QPInfo));
    check (remote_qp_info != NULL, "Failed to allocate remote_qp_info");

    for (i = 0; i < num_peers; i++) {
	ret = sock_get_qp_info (peer_sockfd[i], &remote_qp_info[i]);
	check (ret == 0, "Failed to get qp_info[%d] from server", i);
    }
    
    /* 步骤5: 根据 rank 匹配，将所有 QP 转为 RTS */
    int peer_ind = -1;
    int j        = 0;
    log (LOG_SUB_HEADER, "IB Config");
    for (i = 0; i < num_peers; i++) {
	peer_ind = -1;
	for (j = 0; j < num_peers; j++) {
	    if (remote_qp_info[j].rank == i) {
		peer_ind = j;
		break;
	    }
	}
	ret = modify_qp_to_rts (ib_res.qp[peer_ind], 
				remote_qp_info[i].qp_num, 
				remote_qp_info[i].lid);
	check (ret == 0, "Failed to modify qp[%d] to rts", peer_ind);
    
	log ("\tqp[%"PRIu32"] <-> qp[%"PRIu32"]", 
	     ib_res.qp[peer_ind]->qp_num, remote_qp_info[i].qp_num);
    }
    log (LOG_SUB_HEADER, "End of IB Config");

    /* 步骤6: 同步确认 (Client 先写 sync，再等 Server 回应) */
    for (i = 0; i < num_peers; i++) {
	n = sock_write (peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
	check (n == sizeof(SOCK_SYNC_MSG), "Failed to write sync to client[%d]", i);
    }
    
    for (i = 0; i < num_peers; i++) {
	n = sock_read (peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
	check (n == sizeof(SOCK_SYNC_MSG), "Failed to receive sync from client");
    }

    /* 步骤7: 关闭 TCP 连接 */
    for (i = 0; i < num_peers; i++) {
	close (peer_sockfd[i]);
    }
    free (peer_sockfd);

    free (local_qp_info);
    free (remote_qp_info);
    return 0;

 error:
    if (peer_sockfd != NULL) {
	for (i = 0; i < num_peers; i++) {
	    if (peer_sockfd[i] > 0) {
		close (peer_sockfd[i]);
	    }
	}
	free (peer_sockfd);
    }

    if (local_qp_info != NULL) {
	free (local_qp_info);
    }

    if (remote_qp_info != NULL) {
	free (remote_qp_info);
    }
    
    return -1;
}

/*
 * setup_ib - 创建所有 RDMA 资源并建立连接 (阶段1 + 阶段2)
 *
 * 【RDMA 资源创建的完整流程】
 *
 *  ┌─────────────────────────────────────────────────────┐
 *  │  1. ibv_get_device_list  获取系统中的 IB 设备列表    │
 *  │  2. ibv_open_device      打开第一个 IB 设备          │
 *  │  3. ibv_alloc_pd         分配保护域                  │
 *  │  4. ibv_query_port       查询端口属性 (获取 LID)     │
 *  │  5. memalign + ibv_reg_mr 分配并注册内存区域          │
 *  │  6. ibv_query_device     查询设备能力 (最大队列深度)  │
 *  │  7. ibv_create_cq        创建完成队列                │
 *  │  8. ibv_create_srq       创建共享接收队列            │
 *  │  9. ibv_create_qp × N    创建 N 个队列对             │
 *  │ 10. connect_qp_*         交换信息 + QP 状态转换      │
 *  └─────────────────────────────────────────────────────┘
 *
 * 完成后所有 QP 都处于 RTS 状态，可以开始 RDMA 数据传输。
 */
int setup_ib ()
{
    int	ret		         = 0;
    int i                        = 0;
    struct ibv_device **dev_list = NULL;    
    memset (&ib_res, 0, sizeof(struct IBRes));

    /* 确定 QP 数量:
     * Server 端需要为每个 Client 创建一个 QP (一对一连接)
     * Client 端需要为每个 Server 创建一个 QP */
    if (config_info.is_server) {
	ib_res.num_qps = config_info.num_clients;
    } else {
	ib_res.num_qps = config_info.num_servers;
    }

    /* ========== 步骤1: 获取 IB 设备列表 ==========
     * ibv_get_device_list 返回系统中所有 IB 设备的指针数组。
     * 参数 NULL 表示不需要设备数量。 */
    dev_list = ibv_get_device_list(NULL);
    check(dev_list != NULL, "Failed to get ib device list.");

    /* ========== 步骤2: 打开 IB 设备 ==========
     * ibv_open_device 打开列表中的第一个设备 (*dev_list)。
     * 返回的 ctx 是后续所有操作的基础——几乎所有 ibv_* 函数都需要它。
     * 类似于文件操作中的 fd (file descriptor)。 */
    ib_res.ctx = ibv_open_device(*dev_list);
    check(ib_res.ctx != NULL, "Failed to open ib device.");

    /* ========== 步骤3: 分配保护域 (PD) ==========
     * PD 是 RDMA 的安全隔离机制。
     * 同一个 PD 下的 QP 只能访问同一个 PD 下注册的 MR。
     * 不同 PD 之间的资源互相隔离，防止越权访问。
     * 类似于操作系统中的进程地址空间隔离。 */
    ib_res.pd = ibv_alloc_pd(ib_res.ctx);
    check(ib_res.pd != NULL, "Failed to allocate protection domain.");

    /* ========== 步骤4: 查询端口属性 ==========
     * 获取 IB 端口的运行时信息，最重要的是 LID (Local Identifier)。
     * LID 类似于 IB 子网中的 MAC 地址，IB 交换机用它来路由数据包。
     * 每个端口在子网管理器 (SM) 配置后会被分配一个唯一的 LID。 */
    ret = ibv_query_port(ib_res.ctx, IB_PORT, &ib_res.port_attr);
    check(ret == 0, "Failed to query IB port information.");
    
    /* ========== 步骤5: 分配内存并注册为 MR ==========
     *
     * 【为什么需要注册 MR？】
     * 普通用户态内存不能直接被 HCA 的 DMA 引擎访问，原因：
     *   1) 虚拟地址需要翻译为物理地址 (HCA 不走 CPU 的 MMU)
     *   2) 页面可能被换出到磁盘 (HCA 需要物理内存始终在位)
     *   3) 需要权限检查 (防止恶意应用访问其他进程的内存)
     *
     * ibv_reg_mr 完成以下工作:
     *   1) 将虚拟页面锁定在物理内存中 (pin memory)，防止换出
     *   2) 在 HCA 中建立虚拟地址到物理地址的映射表
     *   3) 返回 lkey (本地访问密钥) 和 rkey (远程访问密钥)
     *
     * 【缓冲区布局】
     * buf_size = msg_size × num_concurr_msgs × num_qps
     * 整个缓冲区被循环使用，每条消息占 msg_size 字节。
     * 由于使用了 SRQ，所有 QP 的接收和发送共享同一个缓冲区。
     *
     * memalign(4096, size): 分配 4096 字节对齐的内存。
     * 4096 = 页大小，对齐可以避免跨页 DMA 的性能损失。
     */
    ib_res.ib_buf_size = config_info.msg_size * config_info.num_concurr_msgs * ib_res.num_qps;
    ib_res.ib_buf      = (char *) memalign (4096, ib_res.ib_buf_size);
    check (ib_res.ib_buf != NULL, "Failed to allocate ib_buf");

    ib_res.mr = ibv_reg_mr (ib_res.pd, (void *)ib_res.ib_buf,
			    ib_res.ib_buf_size,
			    IBV_ACCESS_LOCAL_WRITE |    /* HCA 可写入 (接收时需要) */
			    IBV_ACCESS_REMOTE_READ |    /* 对端可 RDMA Read */
			    IBV_ACCESS_REMOTE_WRITE);   /* 对端可 RDMA Write */
    check (ib_res.mr != NULL, "Failed to register mr");
    
    /* ========== 步骤6: 查询设备属性 ==========
     * 获取 HCA 的硬件能力参数，如:
     *   max_cqe:    CQ 最大可容纳的 CQE(完成队列条目) 数
     *   max_qp_wr:  每个 QP 的 SQ/RQ 最大 WR 数
     *   max_srq_wr: SRQ 最大 WR 数
     * 这些值在后续创建 CQ/QP/SRQ 时用作容量参数。 */
    ret = ibv_query_device(ib_res.ctx, &ib_res.dev_attr);
    check(ret==0, "Failed to query device");
    
    /* ========== 步骤7: 创建完成队列 (CQ) ==========
     * CQ 是"收割"已完成操作的地方。每当一个 Send 或 Recv 操作完成，
     * HCA 会在 CQ 中写入一个 WC (Work Completion)。
     * 用户通过 ibv_poll_cq 从 CQ 中读取 WC 来获知操作结果。
     *
     * 本项目所有 QP 共用一个 CQ，简化了完成事件的处理。
     * max_cqe 设为设备支持的最大值，避免 CQ 溢出。
     *
     * 参数说明:
     *   ctx:          设备上下文
     *   max_cqe:      最大 CQE 容量
     *   cq_context:   NULL (用户上下文，poll 时可取回)
     *   channel:      NULL (不使用事件通知模式，采用轮询)
     *   comp_vector:  0 (完成事件的中断向量号) */
    ib_res.cq = ibv_create_cq (ib_res.ctx, ib_res.dev_attr.max_cqe, 
			       NULL, NULL, 0);
    check (ib_res.cq != NULL, "Failed to create cq");

    /* ========== 步骤8: 创建共享接收队列 (SRQ) ==========
     *
     * 【SRQ vs 普通 RQ】
     * 普通模式: 每个 QP 有自己的 RQ，需要分别预投递 Recv WR
     *   - N 个 QP，每个预投递 M 个 Recv WR → 总共 N×M 个 Recv WR
     *
     * SRQ 模式: 所有 QP 共享一个 SRQ，Recv WR 集中管理
     *   - 只需投递一个较小的 WR 池，动态分配给各 QP
     *   - 优点: 减少总 Recv WR 数量，降低内存开销
     *   - 特别适合: 多连接但每个连接流量不均的场景
     *
     * max_wr:  SRQ 最大可容纳的 Recv WR 数
     * max_sge: 每个 Recv WR 最多包含的 SGE 数 (这里设 1) */
    struct ibv_srq_init_attr srq_init_attr = {
	.attr.max_wr  = ib_res.dev_attr.max_srq_wr,
	.attr.max_sge = 1,
    };

    ib_res.srq = ibv_create_srq (ib_res.pd, &srq_init_attr);
    check (ib_res.srq != NULL, "Failed to create srq");

    /* ========== 步骤9: 创建队列对 (QP) ==========
     *
     * QP 是 RDMA 通信的核心抽象，每个 QP 包含:
     *   - SQ (Send Queue): 存放发送请求 (Send WR)
     *   - RQ (Receive Queue): 存放接收请求 (Recv WR)
     *     (本项目 RQ 由 SRQ 替代，所以实际 RQ 为空壳)
     *
     * 每个 QP 代表一个端到端的连接 (类似 TCP socket)。
     * RC (Reliable Connection) 类型的 QP 保证:
     *   - 数据可靠传输 (有 ACK 确认和重传机制)
     *   - 数据按序到达
     *   - 类似 TCP 的可靠性，但绕过了内核协议栈
     *
     * send_cq / recv_cq: 发送/接收完成事件分别投递到哪个 CQ
     *   (这里都设为同一个 CQ，简化处理)
     * srq: 关联的 SRQ，所有 QP 共享同一个 SRQ
     * qp_type: IBV_QPT_RC = Reliable Connection */
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = ib_res.cq,
        .recv_cq = ib_res.cq,
	.srq     = ib_res.srq,
        .cap = {
            .max_send_wr = ib_res.dev_attr.max_qp_wr,
            .max_recv_wr = ib_res.dev_attr.max_qp_wr,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };

    /* 分配 QP 指针数组，然后逐个创建 QP */
    ib_res.qp = (struct ibv_qp **) calloc (ib_res.num_qps, 
					   sizeof(struct ibv_qp *));
    check (ib_res.qp != NULL, "Failed to allocate qp");

    for (i = 0; i < ib_res.num_qps; i++) {
	ib_res.qp[i] = ibv_create_qp (ib_res.pd, &qp_init_attr);
	check (ib_res.qp[i] != NULL, "Failed to create qp[%d]", i);
    }

    /* ========== 步骤10: 建立 QP 连接 ==========
     * 通过 TCP 交换 QP 信息，然后将 QP 状态转换为 RTS。
     * 完成后 QP 就绪，可以进行 RDMA 数据传输。 */
    if (config_info.is_server) {
	ret = connect_qp_server ();
    } else {
	ret = connect_qp_client ();
    }
    check (ret == 0, "Failed to connect qp");

    ibv_free_device_list (dev_list);
    return 0;

 error:
    if (dev_list != NULL) {
	ibv_free_device_list (dev_list);
    }
    return -1;
}

/*
 * close_ib_connection - 按照创建逆序销毁所有 RDMA 资源
 *
 * 【销毁顺序必须严格遵守，否则会报错】
 *
 *  1. 销毁 QP (因为 QP 引用了 CQ、SRQ、PD)
 *  2. 销毁 SRQ (引用了 PD)
 *  3. 销毁 CQ (引用了 ctx)
 *  4. 注销 MR (引用了 PD，且会解除内存锁定)
 *  5. 释放 PD (引用了 ctx)
 *  6. 关闭设备 ctx
 *  7. 释放用户态缓冲区 (MR 注销后才能 free)
 *
 * 如果不按此顺序，ibv_destroy_* 会因为资源仍被引用而返回 EBUSY 错误。
 */
void close_ib_connection ()
{
    int i;

    /* 1. 销毁所有 QP */
    if (ib_res.qp != NULL) {
	for (i = 0; i < ib_res.num_qps; i++) {
	    if (ib_res.qp[i] != NULL) {
		ibv_destroy_qp (ib_res.qp[i]);
	    }
	}
	free (ib_res.qp);
    }

    /* 2. 销毁 SRQ */
    if (ib_res.srq != NULL) {
	ibv_destroy_srq (ib_res.srq);
    }

    /* 3. 销毁 CQ */
    if (ib_res.cq != NULL) {
	ibv_destroy_cq (ib_res.cq);
    }

    /* 4. 注销 MR (解除内存锁定) */
    if (ib_res.mr != NULL) {
	ibv_dereg_mr (ib_res.mr);
    }

    /* 5. 释放 PD */
    if (ib_res.pd != NULL) {
        ibv_dealloc_pd (ib_res.pd);
    }

    /* 6. 关闭设备上下文 */
    if (ib_res.ctx != NULL) {
        ibv_close_device (ib_res.ctx);
    }

    /* 7. 释放用户态数据缓冲区 */
    if (ib_res.ib_buf != NULL) {
	free (ib_res.ib_buf);
    }
}
