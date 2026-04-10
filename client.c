/*
 * client.c - RDMA Echo Client 实现
 *
 * ============================================================================
 * 【Client 端在 RDMA 通信链路中的角色】
 * ============================================================================
 *
 * Client 端是 Echo 应用的"主动方"，负责发起 Echo 请求并等待回应。
 *
 * 完整流程:
 *
 *  1. 预投递接收请求 (pre-post recv)
 *     └── 在 SRQ 中投递 Recv WR，准备接收 Server 的 START 信号和 Echo 回送
 *
 *  2. 等待 START 信号
 *     └── 轮询 CQ 等待收到 imm_data==MSG_CTL_START 的消息
 *
 *  3. 发起第一批 Echo 请求 (pre-post sends)
 *     └── 向每个 Server 的每个并发槽位发送一条消息
 *         imm_data = 本 Client 的 rank (让 Server 知道回送给谁)
 *
 *  4. 主循环: 收到 Echo 回送 → 再发一条 → 补充 Recv WR
 *     ┌──────────────────────────────────────────────────────┐
 *     │  ibv_poll_cq → 取出 WC                               │
 *     │  ├── WC.opcode == IBV_WC_RECV:                       │
 *     │  │   ├── imm_data == MSG_CTL_STOP → 停止             │
 *     │  │   └── 否则: post_send(Echo) + post_srq_recv(补充) │
 *     │  └── WC.opcode == IBV_WC_SEND: 忽略                  │
 *     └──────────────────────────────────────────────────────┘
 *
 *  5. 收到所有 Server 的 STOP 信号后退出
 *
 * 【Echo 的"乒乓球"模式】
 *   Client发 → Server回 → Client发 → Server回 → ...
 *   每收到一条回送，就立即再发一条新消息，保持并发度恒定。
 *   这种模式下，每次 Round-Trip 测量一个 Echo 延迟。
 *
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>

#include "debug.h"
#include "config.h"
#include "setup_ib.h"
#include "ib.h"
#include "client.h"

/*
 * client_thread_func - Client 工作线程
 *
 * 与 server_thread 对称，但有以下关键差异:
 *   - Client 是 Echo 发起者 (先 send)，Server 是 Echo 响应者 (先 recv)
 *   - Client 等待 START 信号才开始发送
 *   - Client 通过 STOP 信号判断何时停止
 */
void *client_thread_func (void *arg)
{
    int         ret		 = 0, n = 0, i = 0, j = 0;
    long	thread_id	 = (long) arg;
    int         msg_size	 = config_info.msg_size;
    int         num_concurr_msgs = config_info.num_concurr_msgs;
    int         num_peers        = ib_res.num_qps;    /* 对端 (Server) 数量 */

    pthread_t   self;
    cpu_set_t   cpuset;

    int                  num_wc		= 20;
    struct ibv_qp	**qp		= ib_res.qp;
    struct ibv_cq       *cq		= ib_res.cq;
    struct ibv_srq      *srq            = ib_res.srq;
    struct ibv_wc       *wc		= NULL;
    uint32_t             lkey           = ib_res.mr->lkey;

    /* 环形缓冲区管理 */
    char		*buf_ptr	= ib_res.ib_buf;
    char		*buf_base	= ib_res.ib_buf;
    int			 buf_offset	= 0;
    size_t               buf_size	= ib_res.ib_buf_size;
    
    uint32_t		imm_data	= 0;
    int			num_acked_peers = 0;
    bool		start_sending	= false;  /* 是否收到 START 信号 */
    bool		stop		= false;
    struct timeval      start, end;
    long                ops_count	= 0;
    double              duration	= 0.0;
    double              throughput	= 0.0;

    /* 设置线程 CPU 亲和性 */
    CPU_ZERO (&cpuset);
    CPU_SET  ((int)thread_id, &cpuset);
    self = pthread_self ();
    ret  = pthread_setaffinity_np (self, sizeof(cpu_set_t), &cpuset);
    check (ret == 0, "thread[%ld]: failed to set thread affinity", thread_id);

    /* ================================================================
     * 阶段1: 预投递接收请求
     * ================================================================
     * 与 Server 端相同的逻辑: 在 SRQ 中预投递 Recv WR。
     * 这些 Recv WR 用于接收:
     *   - Server 的 START 控制信号
     *   - Server 的 Echo 回送消息
     *   - Server 的 STOP 控制信号
     */
    wc = (struct ibv_wc *) calloc (num_wc, sizeof(struct ibv_wc));
    check (wc != NULL, "thread[%ld]: failed to allocate wc.", thread_id);

    for (i = 0; i < num_peers; i++) {
	for (j = 0; j < num_concurr_msgs; j++) {
	    ret = post_srq_recv (msg_size, lkey, (uint64_t)buf_ptr, srq, buf_ptr);
	    buf_offset = (buf_offset + msg_size) % buf_size;
	    buf_ptr = buf_base + buf_offset;
	}
    }

    /* ================================================================
     * 阶段2: 等待 START 信号
     * ================================================================
     * 阻塞式轮询 CQ，直到从所有 Server 收到 MSG_CTL_START。
     * 
     * 使用 do-while 确保至少 poll 一次，且只在 n>=1 时继续。
     * 收到 START 的 Recv WC 后:
     *   1) 补充一个 Recv WR (因为刚消费了一个)
     *   2) 检查是否所有 Server 都已发来 START
     */
    while (start_sending != true) {
        do {
            n = ibv_poll_cq (cq, num_wc, wc);
        } while (n < 1);
        check (n > 0, "thread[%ld]: failed to poll cq", thread_id);

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                check (0, "thread[%ld]: wc failed status: %s.",
                       thread_id, ibv_wc_status_str(wc[i].status));
            }
            if (wc[i].opcode == IBV_WC_RECV) {
                /* 补充 Recv WR */
                post_srq_recv (msg_size, lkey, wc[i].wr_id, srq, (char *)wc[i].wr_id);
                
                if (ntohl(wc[i].imm_data) == MSG_CTL_START) {
		    num_acked_peers += 1;
		    if (num_acked_peers == num_peers) {
			start_sending = true;  /* 所有 Server 都已就绪 */
			break;
		    }
                }
            }
        }
    }
    log ("thread[%ld]: ready to send", thread_id);

    /* ================================================================
     * 阶段3: 发起第一批 Echo 请求 (Pre-post Sends)
     * ================================================================
     * 向每个 Server 发送 num_concurr_msgs 条消息。
     * imm_data 设为 Server 的索引 i (即对端 rank)，
     * 这样 Server 知道通过 qp[imm_data] 回送给哪个 Client。
     *
     * 这批初始 Send 启动了 Echo 的"乒乓球"模式:
     *   Client Send → Server Recv → Server Send (Echo) → Client Recv → Client Send → ...
     *
     * 并发度 = num_peers × num_concurr_msgs
     * 即网络中同时有这么多条消息在飞行 (in-flight)。
     */
    buf_offset = 0;
    debug ("buf_ptr = %"PRIx64"", (uint64_t)buf_ptr);
    for (i = 0; i < num_peers; i++) {
	for (j = 0; j < num_concurr_msgs; j++) {
	    ret = post_send (msg_size, lkey, (uint64_t)buf_ptr, (uint32_t)i, qp[i], buf_ptr);
	    check (ret == 0, "thread[%ld]: failed to post send", thread_id);
	    buf_offset = (buf_offset + msg_size) % buf_size;
	    buf_ptr = buf_base + buf_offset;
	}
    }

    /* ================================================================
     * 阶段4: 主 Echo 循环
     * ================================================================
     * 与 Server 端类似，但 Client 端的行为略有不同:
     *
     * 收到 Recv WC 时:
     *   - 如果 imm_data == MSG_CTL_STOP → Server 说停止
     *   - 否则: 这是 Server 的 Echo 回送
     *     → post_send: 立即再发一条新消息 (维持并发度)
     *     → post_srq_recv: 补充 Recv WR
     *
     * 【"乒乓球"并发度维持】
     * 初始投递了 N 条 Send。每收到一条 Echo 回送就再发一条。
     * 这样网络中始终保持 N 条消息在飞行 (pipeline)，
     * 实现了高吞吐的流水线效果。
     */
    num_acked_peers = 0;
    while (stop != true) {
        n = ibv_poll_cq (cq, num_wc, wc);
        if (n < 0) {
            check (0, "thread[%ld]: Failed to poll cq", thread_id);
        }

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].opcode == IBV_WC_SEND) {
                    check (0, "thread[%ld]: send failed status: %s; wr_id = %"PRIx64"",
                           thread_id, ibv_wc_status_str(wc[i].status), wc[i].wr_id);
                } else {
                    check (0, "thread[%ld]: recv failed status: %s; wr_id = %"PRIx64"",
                           thread_id, ibv_wc_status_str(wc[i].status), wc[i].wr_id);
                }
            }

	    if (wc[i].opcode == IBV_WC_RECV) {
                ops_count += 1;
                debug ("ops_count = %ld", ops_count);

                /* 预热阶段结束，开始计时 */
                if (ops_count == NUM_WARMING_UP_OPS) {
                    gettimeofday (&start, NULL);
                }

                /* 从 WC 里取出两条关键信息:
                 *   imm_data: 对端在 post_send 时写的 32 位 imm, 需做字节序转换
                 *             本项目中: Server 回送时保持 imm = 对端 Server 的索引,
                 *             Client 用它作为 qp[] 的下标把消息再发回同一个 Server。
                 *   wr_id:    我们当初 post_srq_recv 时把缓冲区地址塞进了 wr_id,
                 *             所以这里转回 char* 就能直接拿到数据所在内存位置,
                 *             省去了"wr_id→buffer" 的映射表。 */
		imm_data = ntohl(wc[i].imm_data);
		char *msg_ptr = (char *)wc[i].wr_id;

                if (imm_data == MSG_CTL_STOP) {
                    /* 收到 Server 的停止信号 */
		    num_acked_peers += 1;
		    if (num_acked_peers == num_peers) {
			gettimeofday (&end, NULL);
			stop = true;
			break;
		    }
                } else {
                    /* 收到 Echo 回送 → 立即再发一条新消息
                     * imm_data 保持为对端 Server 的索引，
                     * 通过 qp[imm_data] 发送到正确的 Server */
		    post_send (msg_size, lkey, 0, imm_data, qp[imm_data], msg_ptr);
		}

                /* 补充 Recv WR，复用刚消费的缓冲区 */
		ret = post_srq_recv (msg_size, lkey, wc[i].wr_id, srq, msg_ptr);
            }
        } /* 遍历所有 WC */
    }

    /* 计算并输出吞吐率统计 */
    duration   = (double)((end.tv_sec - start.tv_sec) * 1000000 + 
			  (end.tv_usec - start.tv_usec));
    throughput = (double)(ops_count) / duration;
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
 * run_client - Client 入口函数
 *
 * 创建工作线程并等待其完成 (与 run_server 结构完全相同)。
 */
int run_client ()
{
    int		ret	    = 0;
    long	num_threads = 1;
    long	i	    = 0;
    
    pthread_t	   *client_threads = NULL;
    pthread_attr_t  attr;
    void	   *status;

    log (LOG_SUB_HEADER, "Run Client");
    
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

    client_threads = (pthread_t *) calloc (num_threads, sizeof(pthread_t));
    check (client_threads != NULL, "Failed to allocate client_threads.");

    for (i = 0; i < num_threads; i++) {
	ret = pthread_create (&client_threads[i], &attr, 
			      client_thread_func, (void *)i);
	check (ret == 0, "Failed to create client_thread[%ld]", i);
    }

    bool thread_ret_normally = true;
    for (i = 0; i < num_threads; i++) {
	ret = pthread_join (client_threads[i], &status);
	check (ret == 0, "Failed to join client_thread[%ld].", i);
	if ((long)status != 0) {
            thread_ret_normally = false;
            log ("thread[%ld]: failed to execute", i);
        }
    }

    if (thread_ret_normally == false) {
        goto error;
    }

    pthread_attr_destroy (&attr);
    free (client_threads);
    return 0;

 error:
    if (client_threads != NULL) {
        free (client_threads);
    }
    
    pthread_attr_destroy (&attr);
    return -1;
}
