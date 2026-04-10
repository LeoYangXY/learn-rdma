/*
 * server.c - RDMA Echo Server 实现
 *
 * ============================================================================
 * 【Server 端在 RDMA 通信链路中的角色】
 * ============================================================================
 *
 * Server 端是 Echo 应用的"被动方"，整体流程:
 *
 *  1. 预投递接收请求 (pre-post recv)
 *     └── 在 SRQ 中投递足够的 Recv WR，准备接收 Client 发来的数据
 *
 *  2. 发送 START 控制信号
 *     └── 通过 SEND_WITH_IMM(imm_data=MSG_CTL_START) 告知所有 Client 可以开始
 *
 *  3. 主循环: 轮询 CQ → 处理 WC → Echo 回送
 *     ┌──────────────────────────────────────────────────────┐
 *     │  ibv_poll_cq(cq) → 取出 WC                          │
 *     │  ├── WC.opcode == IBV_WC_RECV (收到消息)             │
 *     │  │   ├── 从 imm_data 获取发送者 rank                 │
 *     │  │   ├── 从 wr_id 获取数据缓冲区地址                 │
 *     │  │   ├── post_send() 将数据原样发回 (Echo)           │
 *     │  │   └── post_srq_recv() 补充一个新的 Recv WR        │
 *     │  └── WC.opcode == IBV_WC_SEND (发送完成)             │
 *     │      └── 可忽略 (只在等待 STOP 确认时才关注)         │
 *     └──────────────────────────────────────────────────────┘
 *
 *  4. 达到 TOT_NUM_OPS 次后，发送 STOP 信号
 *     └── SEND_WITH_IMM(imm_data=MSG_CTL_STOP, wr_id=IB_WR_ID_STOP)
 *
 *  5. 等待所有 STOP 的 Send WC 确认
 *
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>

#include "debug.h"
#include "ib.h"
#include "setup_ib.h"
#include "config.h"
#include "server.h"

/*
 * server_thread - Server 工作线程
 *
 * 本项目中只启动 1 个线程处理所有连接的消息。
 * 所有 QP 共享同一个 CQ 和 SRQ，因此一个线程就能处理所有 QP 的收发。
 *
 * 【完成队列 (CQ) 轮询模型】
 * 本项目采用"忙轮询"(busy polling) 模型:
 *   while (true) {
 *       n = ibv_poll_cq(cq, batch_size, wc_array);
 *       处理 n 个 WC;
 *   }
 *
 * 特点:
 *   - 延迟极低 (微秒级)，因为 CPU 一直在检查 CQ
 *   - CPU 占用率 100%，适合对延迟敏感的高性能场景
 *   - 替代方案: 事件通知模型 (ibv_get_cq_event)，CPU 友好但延迟更高
 */
void *server_thread (void *arg)
{
    int         ret		 = 0, i = 0, j = 0, n = 0;
    long        thread_id	 = (long) arg;
    int         num_concurr_msgs = config_info.num_concurr_msgs; /* 每个 QP 的并发消息数 */
    int         msg_size	 = config_info.msg_size;         /* 每条消息大小 (字节) */
    int         num_peers        = ib_res.num_qps;               /* 对端 (Client) 数量 */

    pthread_t   self;
    cpu_set_t   cpuset;

    int                  num_wc		= 20;   /* 每次 poll_cq 最多取 20 个 WC */
    struct ibv_qp       **qp		= ib_res.qp;
    struct ibv_cq       *cq		= ib_res.cq;
    struct ibv_srq      *srq            = ib_res.srq;
    struct ibv_wc       *wc             = NULL;   /* WC (Work Completion) 数组 */
    uint32_t             lkey           = ib_res.mr->lkey; /* MR 的本地访问密钥 */
    
    /* 缓冲区管理变量 */
    char                *buf_ptr	= ib_res.ib_buf;       /* 当前缓冲区指针 */
    char                *buf_base	= ib_res.ib_buf;       /* 缓冲区基地址 */
    int                  buf_offset	= 0;                   /* 当前偏移量 */
    size_t               buf_size	= ib_res.ib_buf_size;  /* 缓冲区总大小 */
    
    uint32_t            imm_data	= 0;
    int			num_acked_peers = 0;    /* 已确认的 STOP 信号数 */
    bool                stop            = false;
    struct timeval      start, end;
    long                ops_count	= 0;    /* 已完成的 Echo 操作计数 */
    double              duration	= 0.0;
    double              throughput	= 0.0;

    /* 分配 WC 数组: 每次 ibv_poll_cq 会把完成通知批量写入此数组
     * WC (Work Completion) 是 HCA 对已完成 WR 的"回执"，字段见主循环处说明 */
    wc = (struct ibv_wc *) calloc (num_wc, sizeof(struct ibv_wc));
    check (wc != NULL, "thread[%ld]: failed to allocate wc.", thread_id);

    /* 设置线程 CPU 亲和性: 将线程绑定到特定 CPU 核
     * 目的: 减少 CPU 缓存失效和线程迁移开销，提升性能 */
    CPU_ZERO (&cpuset);
    CPU_SET  ((int)thread_id, &cpuset);
    self = pthread_self ();
    ret  = pthread_setaffinity_np (self, sizeof(cpu_set_t), &cpuset);
    check (ret == 0, "thread[%ld]: failed to set thread affinity", thread_id);

    /* ================================================================
     * 阶段1: 预投递接收请求 (Pre-post Recv WR)
     * ================================================================
     *
     * 【为什么要预投递？】
     * RDMA 的接收操作是"被动触发"的:
     *   - 对端 post_send 后，数据包到达本端 HCA
     *   - HCA 从 SRQ 中取一个 Recv WR，将数据 DMA 写入其指定的缓冲区
     *   - 如果 SRQ 中没有可用的 Recv WR，HCA 会返回 RNR NAK (Receiver Not Ready)
     *
     * 所以必须在对端发送之前，提前投递足够的 Recv WR。
     * 投递数量 = num_peers × num_concurr_msgs
     * (确保每个 QP 的每条并发消息都有对应的接收缓冲区)
     *
     * 【缓冲区分配策略】
     * 使用环形缓冲区: buf_offset 循环递增
     * 每个 Recv WR 指向 buf_base + buf_offset 处的 msg_size 字节
     * wr_id 设为缓冲区地址，方便 WC 中定位数据
     */
    for (i = 0; i < num_peers; i++) {
        for (j = 0; j < num_concurr_msgs; j++) {
            ret = post_srq_recv (msg_size, lkey, (uint64_t)buf_ptr, srq, buf_ptr);
            buf_offset = (buf_offset + msg_size) % buf_size;  /* 环形递进 */
            buf_ptr = buf_base + buf_offset;
        }
    }

    /* ================================================================
     * 阶段2: 发送 START 控制信号
     * ================================================================
     * 向每个 Client 发送一个 imm_data=MSG_CTL_START 的空消息。
     * Client 收到此消息后才开始发送 Echo 数据。
     * 这确保了: Server 的 Recv WR 已经就位 → Client 才开始发送。
     */
    for (i = 0; i < num_peers; i++) {
	ret = post_send (0, lkey, 0, MSG_CTL_START, qp[i], buf_base);
	check (ret == 0, "thread[%ld]: failed to signal the client to start", thread_id);
    }

    /* ================================================================
     * 阶段3: 主 Echo 循环 — 收消息 → 回送 → 补充 Recv WR
     * ================================================================
     *
     * 【每次循环的步骤】
     * 1) ibv_poll_cq: 从 CQ 批量取出最多 num_wc 个 WC
     *    - 返回值 n: 取出的 WC 数量 (0 = 队列为空, <0 = 错误)
     *    - 非阻塞: 如果 CQ 为空，立即返回 0
     *
     * 2) 遍历每个 WC:
     *    - 检查 wc.status == IBV_WC_SUCCESS (确认操作成功)
     *    - 判断 wc.opcode:
     *      · IBV_WC_RECV: 收到了消息
     *        → 从 wc.imm_data 获取发送者的 rank
     *        → 从 wc.wr_id 获取数据所在的缓冲区地址
     *        → post_send: 将数据原样发回 (Echo)，imm_data 保持不变
     *        → post_srq_recv: 为该缓冲区补充一个新的 Recv WR
     *      · IBV_WC_SEND: 发送操作完成 (这里不做处理)
     *
     * 【为什么要补充 Recv WR？】
     * SRQ 中的 Recv WR 是一次性的——每收到一条消息就消费一个。
     * 如果不及时补充，最终 SRQ 会被耗尽，导致 RNR NAK。
     * 所以每处理完一个 Recv WC，就立即补充一个新的 Recv WR，
     * 实现"消费一个补一个"的稳态平衡。
     *
     * 【struct ibv_wc 关键字段速查 (初学者必看)】
     *   wc.status    - 操作状态，IBV_WC_SUCCESS 表示成功，其他都是错
     *   wc.opcode    - 是哪种操作完成:
     *                    IBV_WC_RECV          → 收到对端发来的 Send 数据
     *                    IBV_WC_RECV_RDMA_WITH_IMM → 收到 Write with Imm
     *                    IBV_WC_SEND          → 本端 Send 已发完
     *                    IBV_WC_RDMA_WRITE    → 本端 Write 已完成
     *                    IBV_WC_RDMA_READ     → 本端 Read 已完成
     *   wc.wr_id     - 原样回传用户在 post 时设置的 wr_id
     *                    本项目里 Recv WR 的 wr_id = 缓冲区地址，可直接复用
     *   wc.imm_data  - 立即数，对端 Send with Imm 时携带 (仅 Recv 侧有)
     *                    注意: 它是网络字节序，需 ntohl() 转主机序
     *   wc.byte_len  - 实际收到的字节数 (仅 Recv 侧有意义)
     *   wc.qp_num    - 哪个本地 QP 产生的这个完成
     */
    while (stop != true) {
        n = ibv_poll_cq (cq, num_wc, wc);
        if (n < 0) {
            check (0, "thread[%ld]: Failed to poll cq", thread_id);
        }

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].opcode == IBV_WC_SEND) {
                    check (0, "thread[%ld]: send failed status: %s",
                           thread_id, ibv_wc_status_str(wc[i].status));
                } else {
                    check (0, "thread[%ld]: recv failed status: %s",
                           thread_id, ibv_wc_status_str(wc[i].status));
                }
            }
	    
	    if (wc[i].opcode == IBV_WC_RECV) {
                ops_count += 1;
                debug ("ops_count = %ld", ops_count);

                /* 预热阶段结束，开始计时 */
                if (ops_count == NUM_WARMING_UP_OPS) {
                    gettimeofday (&start, NULL);
                }
                /* 达到总操作数，停止 */
                if (ops_count == TOT_NUM_OPS) {
                    gettimeofday (&end, NULL);
                    stop = true;
                    break;
                }

                /* ---- Echo 回送 ----
                 * imm_data 中存储了发送者的 rank (由 Client 在 post_send 时设置)
                 * 使用 rank 作为 qp 数组的索引，确保通过正确的 QP 回送给正确的 Client
                 * msg_ptr 是数据所在的缓冲区地址 (从 wr_id 还原) */
		imm_data = ntohl(wc[i].imm_data);  /* 网络字节序 → 主机字节序 */
                char *msg_ptr = (char *)wc[i].wr_id; /* wr_id 就是缓冲区地址 */
                post_send (msg_size, lkey, 0, imm_data, qp[imm_data], msg_ptr);

                /* ---- 补充 Recv WR ----
                 * 复用刚刚消费的缓冲区，重新投递到 SRQ */
                post_srq_recv (msg_size, lkey, wc[i].wr_id, srq, msg_ptr);
            }
        }
    }

    /* ================================================================
     * 阶段4: 发送 STOP 控制信号
     * ================================================================
     * 使用特殊的 wr_id = IB_WR_ID_STOP 来标记停止消息的 Send WR。
     * 后续需要确认所有 STOP 消息都成功发送出去。
     */
    for (i = 0; i < num_peers; i++) {
	ret = post_send (0, lkey, IB_WR_ID_STOP, MSG_CTL_STOP, qp[i], ib_res.ib_buf);
	check (ret == 0, "thread[%ld]: failed to signal the client to stop", thread_id);
    }

    /* ================================================================
     * 阶段5: 等待所有 STOP 消息的发送完成确认
     * ================================================================
     * 轮询 CQ，直到收到所有 STOP 消息的 Send WC。
     * 通过 wc.wr_id == IB_WR_ID_STOP 来识别 STOP 消息的完成。
     * 确保所有 Client 都收到 STOP 后再退出。
     */
    stop = false;
    while (stop != true) {
        n = ibv_poll_cq (cq, num_wc, wc);
        if (n < 0) {
            check (0, "thread[%ld]: Failed to poll cq", thread_id);
        }

	for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].opcode == IBV_WC_SEND) {
                    check (0, "thread[%ld]: send failed status: %s",
                           thread_id, ibv_wc_status_str(wc[i].status));
                } else {
                    check (0, "thread[%ld]: recv failed status: %s",
                           thread_id, ibv_wc_status_str(wc[i].status));
                }
            }

            if (wc[i].opcode == IBV_WC_SEND) {
                if (wc[i].wr_id == IB_WR_ID_STOP) {
		    num_acked_peers += 1;
		    if (num_acked_peers == num_peers) {
			stop = true;    /* 所有 STOP 都已确认发送成功 */
			break;
		    }
                }
            }
        }
    }
    
    /* 计算并输出吞吐率统计 */
    duration   = (double)((end.tv_sec - start.tv_sec) * 1000000 +
                          (end.tv_usec - start.tv_usec));   /* 微秒 */
    throughput = (double)(ops_count) / duration;             /* Mops/s */
    log ("thread[%ld]: throughput = %f (Mops/s)",  thread_id, throughput);

    free (wc);
    pthread_exit ((void *)0);

 error:
    if (wc != NULL) {
    	free (wc);
    }
    pthread_exit ((void *)-1);
}

/*
 * run_server - Server 入口函数
 *
 * 创建工作线程并等待其完成。
 * 当前只创建 1 个线程，但预留了多线程扩展能力。
 */
int run_server ()
{
    int   ret         = 0;
    long  num_threads = 1;
    long  i           = 0;

    pthread_t           *threads = NULL;
    pthread_attr_t       attr;
    void                *status;

    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

    threads = (pthread_t *) calloc (num_threads, sizeof(pthread_t));
    check (threads != NULL, "Failed to allocate threads.");

    for (i = 0; i < num_threads; i++) {
	ret = pthread_create (&threads[i], &attr, server_thread, (void *)i);
	check (ret == 0, "Failed to create server_thread[%ld]", i);
    }

    /* 等待所有线程结束 */
    bool thread_ret_normally = true;
    for (i = 0; i < num_threads; i++) {
        ret = pthread_join (threads[i], &status);
        check (ret == 0, "Failed to join thread[%ld].", i);
        if ((long)status != 0) {
            thread_ret_normally = false;
            log ("server_thread[%ld]: failed to execute", i);
        }
    }

    if (thread_ret_normally == false) {
        goto error;
    }

    pthread_attr_destroy    (&attr);
    free (threads);

    return 0;

 error:
    if (threads != NULL) {
        free (threads);
    }
    pthread_attr_destroy    (&attr);
    
    return -1;
}
