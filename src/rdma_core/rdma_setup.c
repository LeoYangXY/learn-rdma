/*
 * rdma_setup.c - RDMA 一次性初始化：资源创建 + 连接建立 + 销毁
 *
 * ============================================================================
 * 【本文件做的所有事情，按执行顺序】
 *
 *  setup_ib()             创建 RDMA 资源（10 步）——走内核驱动
 *    └── *_side_setup_connection()  通过 TCP 交换 QPInfo + QP 状态转换
 *          └── modify_qp_to_rts()  QP: RESET → INIT → RTR → RTS（在 rdma_verbs.c）
 *
 *  close_ib_connection()  按逆序销毁所有资源
 * ============================================================================
 */

#include <malloc.h>
#include <sys/socket.h>
#include "common.h"


struct IBRes ib_res;
/* ----------------------------------------------------------------------------
 * 【ib_res.qp 的指针布局】
 *
 *   ib_res                                          内核/驱动里的 ibv_qp 对象
 *  ┌─────────────────────┐                         ┌──────────────────────┐
 *  │ ctx                 │                    ┌──► │ ibv_qp { qp_num=100, │
 *  │ pd                  │                    │    │           ...     }  │
 *  │ mr                  │                    │    └──────────────────────┘
 *  │ cq                  │                    │    ┌──────────────────────┐
 *  │ srq                 │                    │ ┌► │ ibv_qp { qp_num=101} │
 *  │ qp ──┐              │                    │ │  └──────────────────────┘
 *  │ ...  │              │                    │ │  ┌──────────────────────┐
 *  └──────┼──────────────┘                    │ │┌►│ ibv_qp { qp_num=102} │
 *         │                                   │ ││ └──────────────────────┘
 *         │   堆上 calloc 出来的指针数组       │ ││
 *         │  ┌────────────────────────────┐   │ ││
 *         └─►│ qp[0] :  *──────────────── │ ──┘ ││
 *            │ qp[1] :  *──────────────── │ ────┘│
 *            │ qp[2] :  *──────────────── │ ─────┘
 *            └────────────────────────────┘
 *                  ↑
 *                  这才是"数组本体"，
 *                  ib_res.qp 只是指向它的指针
 *
 * 因此 ib_res.qp 是二级指针（ibv_qp **）：
 *   ib_res.qp            → 指针数组首地址
 *   ib_res.qp[i]         → 第 i 个 QP 对象指针（ibv_qp *）
 *   ib_res.qp[i]->qp_num → QP 编号（uint32_t，对端靠它定位本端 QP）
 * ---------------------------------------------------------------------------- */


/* ============================================================================
 * 第一部分: 连接建立（通过 TCP 交换 QP 信息 → QP 转 RTS）
 * ============================================================================
 *
 * 下面的那些sock_*相关的函数，就是借助tcp socket的方式，让local rank和remote rank拿、发信息
 *
 */

static int find_peer_by_rank(struct QPInfo *info, int count, uint32_t target)
{
    for (int j = 0; j < count; j++)
        if (info[j].rank == target) return j;
    return 0;
}

/*
 * server_side_setup_connection - Server 端连接建立
 *
 * 7 个阶段: TCP listen+accept → 准备本地 QPInfo → 收对端 QPInfo → 发本地 QPInfo
 *           → QP 转 RTS → 屏障同步 → 关 TCP
 */
int server_side_setup_connection()
{
    int ret = 0, n = 0, i = 0;
    int num_peers = config_info.num_clients;   /* server 需要等待接入的 client 总数 */
    int sockfd = 0;                            /* 监听 socket，仅用于 accept */
    int *peer_sockfd = NULL;                   /* 与每个 client 的连接 socket（accept 返回） */
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    char sock_buf[64] = {'\0'};

    struct QPInfo *local_qp_info = NULL;//local_qp_info[i]:和rank i 建联的本地qp
    struct QPInfo *remote_qp_info = NULL;//第i个accept进来的client，要看.rank字段去具体确定是哪个rank

    /* === 阶段 1: TCP 监听 + 接受 N 个 client 的连接 ===
     * 标准 TCP server 三连: bind → listen → accept。
     * sockfd 仅作为监听端点存在；peer_sockfd[i] 是 accept 后用于实际收发的连接 socket。
     */
    sockfd = sock_create_bind(config_info.sock_port);
    check(sockfd > 0, "Failed to create server socket.");
    listen(sockfd, 5);
    peer_sockfd = (int *)calloc(num_peers, sizeof(int));
    check(peer_sockfd != NULL, "Failed to allocate peer_sockfd");
    /* 一次 accept = 等一个 client 连进来，连上后返回一个新 fd，后续用它收发数据。
     *
     *   sockfd                ← 一直监听端口，谁来都先经过它（只接客，不通信）
     *      │
     *      ├── accept ──→ peer_sockfd[0]   ← 跟第 1 个连进来的 client 通信
     *      ├── accept ──→ peer_sockfd[1]   ← 跟第 2 个连进来的 client 通信
     *      └── accept ──→ peer_sockfd[2]   ← 跟第 3 个连进来的 client 通信
     *
     * 后面 sock_get_qp_info(peer_sockfd[i], ...) 就是从第 i 个 client 的连接上读数据。
     * 参数 peer_addr / peer_addr_len 用于接收对端 IP+port，本项目并未使用。
     */
    for (i = 0; i < num_peers; i++) {
        peer_sockfd[i] = accept(sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        check(peer_sockfd[i] > 0, "Failed to accept client[%d]", i);
    }


    /* === 阶段 2: 准备本地 N 条 QPInfo ===
     * 每个对端用一条独立 QP 接入，因此每条 QPInfo 的 qp_number 不同。
     * 约定: local_qp_info[k] 对应 ib_res.qp[k]，预留给 rank=k 的 client。
     * lid/rank/gid 是本机属性，对所有条目相同；只有 qp_number 随 i 变。
     */
    local_qp_info = (struct QPInfo *)calloc(num_peers, sizeof(struct QPInfo));
    check(local_qp_info != NULL, "Failed to allocate local_qp_info");
    for (i = 0; i < num_peers; i++) {
        local_qp_info[i].lid    = ib_res.port_attr.lid;
        local_qp_info[i].qp_number = ib_res.qp[i]->qp_num;       /* ★ 第 i 条对应 qp[i] */
        local_qp_info[i].rank   = config_info.rank;
        ibv_query_gid(ib_res.ctx, IB_PORT, 1, &local_qp_info[i].gid);
    }


    /* === 阶段 3: 先收对端 QPInfo === */
    remote_qp_info = (struct QPInfo *)calloc(num_peers, sizeof(struct QPInfo));
    check(remote_qp_info != NULL, "Failed to allocate remote_qp_info");
    for (i = 0; i < num_peers; i++) {
        ret = sock_get_qp_info(peer_sockfd[i], &remote_qp_info[i]);
        check(ret == 0, "Failed to get qp_info from client[%d]", i);
    }


    /* === 阶段 4: 给每个对端发这个server与之相连的链路对应的 QPInfo  === */
    for (i = 0; i < num_peers; i++) {
        int pi = find_peer_by_rank(remote_qp_info, num_peers, i);
        ret = sock_set_qp_info(peer_sockfd[i], &local_qp_info[pi]);
        check(ret == 0, "Failed to send qp_info to client[%d]", i);
    }

    /* === 阶段 5: 把每个 QP 推到 RTS ===
     * 同样按 rank 校正: rank=i 的对端 ↔ 本地 qp[?]，传入对端的 GID/qp_number/lid，
     * QP 状态机走 RESET → INIT → RTR → RTS（细节见 modify_qp_to_rts）。
     */
    log(LOG_SUB_HEADER, "Start of IB Config");
    for (i = 0; i < num_peers; i++) {
        int pi = find_peer_by_rank(remote_qp_info, num_peers, i);
        ret = modify_qp_to_rts(ib_res.qp[pi],
                               remote_qp_info[i].qp_number,
                               remote_qp_info[i].lid,
                               &remote_qp_info[i].gid);
        check(ret == 0, "Failed to modify qp[%d] to RTS", pi);
        log("\tqp[%"PRIu32"] <-> qp[%"PRIu32"]",
            ib_res.qp[pi]->qp_num, remote_qp_info[i].qp_number);
    }
    log(LOG_SUB_HEADER, "End of IB Config");

    /* === 阶段 6: TCP 屏障同步 ===
     * 让双方都确认"对端的 QP 已经处于 RTS 状态、可以收 RDMA 报文了"之后，才放行往下走
     * 为什么需要此：阶段 5 把 QP 推到 RTS 是本地操作——每一方只知道"我自己这边好了"，不知道对端有没有也搞定
     */
    for (i = 0; i < num_peers; i++) {
        n = sock_read(peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
        check(n == sizeof(SOCK_SYNC_MSG), "Failed to receive sync");
    }
    for (i = 0; i < num_peers; i++) {
        n = sock_write(peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
        check(n == sizeof(SOCK_SYNC_MSG), "Failed to write sync");
    }

    /* === 阶段 7: 关闭 TCP，TCP 通道使命完成，后续全走 RDMA === */
    for (i = 0; i < num_peers; i++) close(peer_sockfd[i]);
    free(peer_sockfd);
    close(sockfd);
    return 0;

 error:
    if (peer_sockfd) {
        for (i = 0; i < num_peers; i++)
            if (peer_sockfd[i] > 0) close(peer_sockfd[i]);
        free(peer_sockfd);
    }
    if (sockfd > 0) close(sockfd);
    return -1;
}


int client_side_setup_connection()
{
    int ret = 0, n = 0, i = 0;
    int num_peers = ib_res.num_qps;            /* = config_info.num_servers，要主动连几个 server */
    int *peer_sockfd = NULL;
    char sock_buf[64] = {'\0'};
    struct QPInfo *local_qp_info = NULL;
    struct QPInfo *remote_qp_info = NULL;

    /* === 阶段 1: 主动 TCP connect 到每个 server ===
     * 顺序由配置文件 servers[] 决定，因此 peer_sockfd[i] 必然连接 servers[i]，确定无歧义。
     */
    peer_sockfd = (int *)calloc(num_peers, sizeof(int));
    check(peer_sockfd != NULL, "Failed to allocate peer_sockfd");
    for (i = 0; i < num_peers; i++) {
        peer_sockfd[i] = sock_create_connect(config_info.servers[i], config_info.sock_port);
        check(peer_sockfd[i] > 0, "Failed to connect to server[%d]", i);
    }

    /* === 阶段 2: 准备本地 N 条 QPInfo ===
     * 约定: local_qp_info[k] 对应 ib_res.qp[k]，发给 servers[k]（即 peer_sockfd[k]）。
     */
    local_qp_info = (struct QPInfo *)calloc(num_peers, sizeof(struct QPInfo));
    check(local_qp_info != NULL, "Failed to allocate local_qp_info");
    for (i = 0; i < num_peers; i++) {
        local_qp_info[i].lid    = ib_res.port_attr.lid;
        local_qp_info[i].qp_number = ib_res.qp[i]->qp_num;
        local_qp_info[i].rank   = config_info.rank;
        ibv_query_gid(ib_res.ctx, IB_PORT, 1, &local_qp_info[i].gid);
    }

    /* === 阶段 3: 先发本地 QPInfo ===
     * 直接 local_qp_info[i] → peer_sockfd[i]
     */
    for (i = 0; i < num_peers; i++) {
        ret = sock_set_qp_info(peer_sockfd[i], &local_qp_info[i]);
        check(ret == 0, "Failed to send qp_info[%d] to server", i);
    }

    /* === 阶段 4: 收对端 QPInfo ===
     * remote_qp_info[i] 来自 servers[i]。但 server 选择用哪个 qp 与本端建连，
     * 是由 server 端的 rank 算法决定，所以收到的 qp_number 不一定按 i 顺序排列——
     * 真正的对应靠 remote_qp_info[i].rank 字段（server 自身的 rank）。
     */
    remote_qp_info = (struct QPInfo *)calloc(num_peers, sizeof(struct QPInfo));
    check(remote_qp_info != NULL, "Failed to allocate remote_qp_info");
    for (i = 0; i < num_peers; i++) {
        ret = sock_get_qp_info(peer_sockfd[i], &remote_qp_info[i]);
        check(ret == 0, "Failed to get qp_info[%d] from server", i);
    }

    /* === 阶段 5: 把每个 QP 推到 RTS === */
    log(LOG_SUB_HEADER, "IB Config");
    for (i = 0; i < num_peers; i++) {
        int pi = find_peer_by_rank(remote_qp_info, num_peers, i);
        ret = modify_qp_to_rts(ib_res.qp[pi],
                               remote_qp_info[i].qp_number,
                               remote_qp_info[i].lid,
                               &remote_qp_info[i].gid);
        check(ret == 0, "Failed to modify qp[%d] to RTS", pi);
        log("\tqp[%"PRIu32"] <-> qp[%"PRIu32"]",
            ib_res.qp[pi]->qp_num, remote_qp_info[i].qp_number);
    }
    log(LOG_SUB_HEADER, "End of IB Config");

    /* === 阶段 6: TCP 屏障同步 */
    for (i = 0; i < num_peers; i++) {
        n = sock_write(peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
        check(n == sizeof(SOCK_SYNC_MSG), "Failed to write sync");
    }
    for (i = 0; i < num_peers; i++) {
        n = sock_read(peer_sockfd[i], sock_buf, sizeof(SOCK_SYNC_MSG));
        check(n == sizeof(SOCK_SYNC_MSG), "Failed to receive sync");
    }

    /* === 阶段 7: 关闭 TCP === */
    for (i = 0; i < num_peers; i++) close(peer_sockfd[i]);
    free(peer_sockfd);
    free(local_qp_info);
    free(remote_qp_info);
    return 0;

 error:
    if (peer_sockfd) {
        for (i = 0; i < num_peers; i++)
            if (peer_sockfd[i] > 0) close(peer_sockfd[i]);
        free(peer_sockfd);
    }
    if (local_qp_info) free(local_qp_info);
    if (remote_qp_info) free(remote_qp_info);
    return -1;
}

/* ============================================================================
 * 第二部分: 资源创建与销毁
 * ============================================================================ */

/*
 * setup_ib - 创建所有 RDMA 资源
 *
 * 【10 步流程】
 *  1. ibv_get_device_list  → 找到系统里的 RDMA 网卡
 *  2. ibv_open_device      → 打开它，得到 ctx
 *  3. ibv_alloc_pd         → 创建保护域
 *  4. ibv_query_port       → 查询端口信息
 *  5. memalign+ibv_reg_mr  → 分配内存 + 注册到网卡
 *  6. ibv_query_device     → 查询网卡能力上限
 *  7. ibv_create_cq        → 创建完成队列
 *  8. ibv_create_srq       → 创建共享接收队列
 *  9. ibv_create_qp × N    → 创建 N 个 QP
 * 10. *_side_setup_connection() → TCP 交换 + QP 状态转换
 */
int setup_ib()
{
    int ret = 0, i = 0;
    struct ibv_device **dev_list = NULL;
    memset(&ib_res, 0, sizeof(struct IBRes));

    ib_res.num_qps = config_info.is_server ?
                     config_info.num_clients : config_info.num_servers;

    /* 1. 找到 RDMA 网卡 */
    dev_list = ibv_get_device_list(NULL);
    check(dev_list != NULL, "Failed to get ib device list.");

    /* 2. 打开设备 */
    ib_res.ctx = ibv_open_device(*dev_list);
    check(ib_res.ctx != NULL, "Failed to open ib device.");

    /* 3. 创建保护域 */
    ib_res.pd = ibv_alloc_pd(ib_res.ctx);
    check(ib_res.pd != NULL, "Failed to allocate pd.");

    /* 4. 查询端口 */
    ret = ibv_query_port(ib_res.ctx, IB_PORT, &ib_res.port_attr);
    check(ret == 0, "Failed to query port.");

    /* 5. 分配内存 + 注册 MR（网卡才能 DMA 访问） */
    ib_res.ib_buf_size = config_info.msg_size * config_info.num_concurr_msgs * ib_res.num_qps;
    ib_res.ib_buf = (char *)memalign(4096, ib_res.ib_buf_size);
    check(ib_res.ib_buf != NULL, "Failed to allocate ib_buf");

    ib_res.mr = ibv_reg_mr(ib_res.pd, ib_res.ib_buf, ib_res.ib_buf_size,
                           IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE);
    check(ib_res.mr != NULL, "Failed to register mr");

    /* 6. 查询设备能力 */
    ret = ibv_query_device(ib_res.ctx, &ib_res.dev_attr);
    check(ret == 0, "Failed to query device");

    /* 7. 创建 CQ */
    ib_res.cq = ibv_create_cq(ib_res.ctx, ib_res.dev_attr.max_cqe, NULL, NULL, 0);
    check(ib_res.cq != NULL, "Failed to create cq");

    /* 8. 创建 SRQ */
    struct ibv_srq_init_attr srq_attr = { .attr.max_wr = ib_res.dev_attr.max_srq_wr, .attr.max_sge = 1 };
    ib_res.srq = ibv_create_srq(ib_res.pd, &srq_attr);
    check(ib_res.srq != NULL, "Failed to create srq");

    /* 9. 创建 QP（关联 CQ + SRQ，RC 类型） */
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ib_res.cq,
        .recv_cq = ib_res.cq,
        .srq     = ib_res.srq,
        .cap     = { .max_send_wr = ib_res.dev_attr.max_qp_wr,
                     .max_recv_wr = ib_res.dev_attr.max_qp_wr,
                     .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC,
    };
    ib_res.qp = (struct ibv_qp **)calloc(ib_res.num_qps, sizeof(struct ibv_qp *));
    check(ib_res.qp != NULL, "Failed to allocate qp array");
    for (i = 0; i < ib_res.num_qps; i++) {
        ib_res.qp[i] = ibv_create_qp(ib_res.pd, &qp_attr);
        check(ib_res.qp[i] != NULL, "Failed to create qp[%d]", i);
    }

    /* 10. 连接建立 */
    ret = config_info.is_server ? server_side_setup_connection() : client_side_setup_connection();
    check(ret == 0, "Failed to connect qp");

    ibv_free_device_list(dev_list);
    return 0;

 error:
    if (dev_list) ibv_free_device_list(dev_list);
    return -1;
}

/*
 * close_ib_connection - 按创建的逆序销毁所有资源
 *
 * 为什么要逆序？因为资源间有依赖：QP 引用了 CQ 和 SRQ，
 * 如果先销毁 CQ，再销毁 QP 就会报 EBUSY。
 */
void close_ib_connection()
{
    int i;
    if (ib_res.qp) {
        for (i = 0; i < ib_res.num_qps; i++)
            if (ib_res.qp[i]) ibv_destroy_qp(ib_res.qp[i]);
        free(ib_res.qp);
    }
    if (ib_res.srq) ibv_destroy_srq(ib_res.srq);
    if (ib_res.cq)  ibv_destroy_cq(ib_res.cq);
    if (ib_res.mr)  ibv_dereg_mr(ib_res.mr);
    if (ib_res.pd)  ibv_dealloc_pd(ib_res.pd);
    if (ib_res.ctx) ibv_close_device(ib_res.ctx);
    if (ib_res.ib_buf) free(ib_res.ib_buf);
}
