/*
 * echo_server.c - RDMA Echo Server
 *
 * Server 是 Echo 的"被动方"：收到消息后原样回送。
 *
 * 流程:
 *   1. 预投递 Recv WR 到 SRQ（准备接收）
 *   2. 发送 START 信号告知 Client 可以开始
 *   3. 主循环: poll CQ → 收到消息 → Echo 回送 → 补充 Recv WR
 *   4. 达到 TOT_NUM_OPS 后发送 STOP
 *   5. 等待 STOP 的 Send WC 确认
 */

#define _GNU_SOURCE
#include <sys/time.h>
#include <pthread.h>
#include "common.h"

static void *server_thread(void *arg)
{
    int ret = 0, i = 0, j = 0, n = 0;
    long thread_id = (long)arg;
    int msg_size = config_info.msg_size;
    int num_concurr_msgs = config_info.num_concurr_msgs;
    int num_peers = ib_res.num_qps;

    pthread_t self;
    cpu_set_t cpuset;

    int num_wc = 256;
    struct ibv_qp **qp = ib_res.qp;
    struct ibv_cq *cq = ib_res.cq;
    struct ibv_srq *srq = ib_res.srq;
    struct ibv_wc *wc = NULL;
    uint32_t lkey = ib_res.mr->lkey;

    char *buf_ptr = ib_res.ib_buf;
    char *buf_base = ib_res.ib_buf;
    int buf_offset = 0;
    size_t buf_size = ib_res.ib_buf_size;

    uint32_t imm_data = 0;
    int num_acked_peers = 0;
    bool stop = false;
    struct timeval start, end;
    long ops_count = 0;
    double duration = 0.0, throughput = 0.0;

    wc = (struct ibv_wc *)calloc(num_wc, sizeof(struct ibv_wc));
    check(wc != NULL, "thread[%ld]: failed to allocate wc.", thread_id);

    /* 绑定 CPU */
    CPU_ZERO(&cpuset);
    CPU_SET((int)thread_id, &cpuset);
    self = pthread_self();
    ret = pthread_setaffinity_np(self, sizeof(cpu_set_t), &cpuset);
    check(ret == 0, "thread[%ld]: failed to set affinity", thread_id);

    /* ===== 阶段1: 预投递 Recv WR =====
     * 必须在对端发送之前投递，否则 HCA 返回 RNR NAK */
    for (i = 0; i < num_peers; i++) {
        for (j = 0; j < num_concurr_msgs; j++) {
            ret = post_srq_recv(msg_size, lkey, (uint64_t)buf_ptr, srq, buf_ptr);
            buf_offset = (buf_offset + msg_size) % buf_size;
            buf_ptr = buf_base + buf_offset;
        }
    }

    /* ===== 阶段2: 发送 START 信号 ===== */
    for (i = 0; i < num_peers; i++) {
        ret = post_send(0, lkey, 0, MSG_CTL_START, qp[i], buf_base);
        check(ret == 0, "thread[%ld]: failed to send START", thread_id);
    }

    /* ===== 阶段3: 主 Echo 循环 =====
     *
     * 忙轮询 CQ，处理每个 WC：
     *   - IBV_WC_RECV: 收到消息 → Echo 回送 + 补充 Recv WR
     *   - IBV_WC_SEND: 发送完成 → 忽略 */
    while (stop != true) {
        n = ibv_poll_cq(cq, num_wc, wc);
        if (n < 0) check(0, "thread[%ld]: Failed to poll cq", thread_id);

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                check(0, "thread[%ld]: wc failed: %s",
                      thread_id, ibv_wc_status_str(wc[i].status));
            }

            if (wc[i].opcode == IBV_WC_RECV) {
                ops_count += 1;
                debug("ops_count = %ld", ops_count);

                if (ops_count == NUM_WARMING_UP_OPS)
                    gettimeofday(&start, NULL);
                if (ops_count == TOT_NUM_OPS) {
                    gettimeofday(&end, NULL);
                    stop = true;
                    break;
                }

                /* Echo 回送: 通过 imm_data 找到发送者的 QP */
                imm_data = ntohl(wc[i].imm_data);
                char *msg_ptr = (char *)wc[i].wr_id;
                post_send(msg_size, lkey, 0, imm_data, qp[imm_data], msg_ptr);
                post_srq_recv(msg_size, lkey, wc[i].wr_id, srq, msg_ptr);
            }
        }
    }

    /* ===== 阶段4: 发送 STOP ===== */
    for (i = 0; i < num_peers; i++) {
        ret = post_send(0, lkey, IB_WR_ID_STOP, MSG_CTL_STOP, qp[i], ib_res.ib_buf);
        check(ret == 0, "thread[%ld]: failed to send STOP", thread_id);
    }

    /* ===== 阶段5: 等待 STOP 发送确认 ===== */
    stop = false;
    while (stop != true) {
        n = ibv_poll_cq(cq, num_wc, wc);
        if (n < 0) check(0, "thread[%ld]: Failed to poll cq", thread_id);

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS)
                check(0, "thread[%ld]: wc failed: %s", thread_id, ibv_wc_status_str(wc[i].status));
            if (wc[i].opcode == IBV_WC_SEND && wc[i].wr_id == IB_WR_ID_STOP) {
                num_acked_peers += 1;
                if (num_acked_peers == num_peers) { stop = true; break; }
            }
        }
    }

    /* 统计 */
    duration = (double)((end.tv_sec - start.tv_sec) * 1000000 +
                        (end.tv_usec - start.tv_usec));
    throughput = (double)(ops_count) / duration;
    log("thread[%ld]: throughput = %f (Mops/s)", thread_id, throughput);

    free(wc);
    pthread_exit((void *)0);

 error:
    if (wc) free(wc);
    pthread_exit((void *)-1);
}

int run_server()
{
    int ret = 0;
    pthread_t tid;
    pthread_attr_t attr;
    void *status;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    ret = pthread_create(&tid, &attr, server_thread, (void *)0L);
    check(ret == 0, "Failed to create server thread");

    ret = pthread_join(tid, &status);
    check(ret == 0, "Failed to join server thread");
    check((long)status == 0, "Server thread failed");

    pthread_attr_destroy(&attr);
    return 0;

 error:
    pthread_attr_destroy(&attr);
    return -1;
}
