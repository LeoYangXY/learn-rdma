/*
 * echo_client.c - RDMA Echo Client
 *
 * Client 是 Echo 的"主动方"：主动发送消息，等待 Server 回送。
 *
 * 流程:
 *   1. 预投递 Recv WR（准备接收 START 和 Echo 回送）
 *   2. 等待 Server 的 START 信号
 *   3. 发起第一批 Send（启动乒乓球）
 *   4. 主循环: 收到 Echo → 再发一条 → 补充 Recv WR
 *   5. 收到 STOP 后停止
 *
 * 【乒乓球并发模式】
 *   初始投 N 条 Send。每收到一条 Echo 回送就再发一条。
 *   网络中始终保持 N 条消息在飞行（pipeline），实现高吞吐。
 */

#define _GNU_SOURCE
#include <sys/time.h>
#include <pthread.h>
#include "common.h"

static void *client_thread(void *arg)
{
    int ret = 0, n = 0, i = 0, j = 0;
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
    bool start_sending = false;
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

    /* ===== 阶段1: 预投递 Recv WR ===== */
    for (i = 0; i < num_peers; i++) {
        for (j = 0; j < num_concurr_msgs; j++) {
            ret = post_srq_recv(msg_size, lkey, (uint64_t)buf_ptr, srq, buf_ptr);
            buf_offset = (buf_offset + msg_size) % buf_size;
            buf_ptr = buf_base + buf_offset;
        }
    }

    /* ===== 阶段2: 等待 START 信号 ===== */
    while (start_sending != true) {
        do { n = ibv_poll_cq(cq, num_wc, wc); } while (n < 1);
        check(n > 0, "thread[%ld]: failed to poll cq", thread_id);

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS)
                check(0, "thread[%ld]: wc failed: %s", thread_id, ibv_wc_status_str(wc[i].status));
            if (wc[i].opcode == IBV_WC_RECV) {
                post_srq_recv(msg_size, lkey, wc[i].wr_id, srq, (char *)wc[i].wr_id);
                if (ntohl(wc[i].imm_data) == MSG_CTL_START) {
                    num_acked_peers += 1;
                    if (num_acked_peers == num_peers) { start_sending = true; break; }
                }
            }
        }
    }
    log("thread[%ld]: ready to send", thread_id);

    /* ===== 阶段3: 发起第一批 Send（启动乒乓球） ===== */
    buf_offset = 0;
    for (i = 0; i < num_peers; i++) {
        for (j = 0; j < num_concurr_msgs; j++) {
            ret = post_send(msg_size, lkey, (uint64_t)buf_ptr, (uint32_t)i, qp[i], buf_ptr);
            check(ret == 0, "thread[%ld]: failed to post send", thread_id);
            buf_offset = (buf_offset + msg_size) % buf_size;
            buf_ptr = buf_base + buf_offset;
        }
    }

    /* ===== 阶段4: 主 Echo 循环 =====
     *
     * 收到 Recv WC:
     *   - imm_data == STOP → 停止
     *   - 否则: Echo 回送 → 再发一条 + 补充 Recv WR
     */
    num_acked_peers = 0;
    while (stop != true) {
        n = ibv_poll_cq(cq, num_wc, wc);
        if (n < 0) check(0, "thread[%ld]: Failed to poll cq", thread_id);

        for (i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                check(0, "thread[%ld]: wc failed: %s; wr_id=%"PRIx64"",
                      thread_id, ibv_wc_status_str(wc[i].status), wc[i].wr_id);
            }

            if (wc[i].opcode == IBV_WC_RECV) {
                ops_count += 1;
                debug("ops_count = %ld", ops_count);

                if (ops_count == NUM_WARMING_UP_OPS)
                    gettimeofday(&start, NULL);

                imm_data = ntohl(wc[i].imm_data);
                char *msg_ptr = (char *)wc[i].wr_id;

                if (imm_data == MSG_CTL_STOP) {
                    num_acked_peers += 1;
                    if (num_acked_peers == num_peers) {
                        gettimeofday(&end, NULL);
                        stop = true;
                        break;
                    }
                } else {
                    /* 收到 Echo → 再发一条（维持并发度） */
                    post_send(msg_size, lkey, 0, imm_data, qp[imm_data], msg_ptr);
                }
                post_srq_recv(msg_size, lkey, wc[i].wr_id, srq, msg_ptr);
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

int run_client()
{
    int ret = 0;
    pthread_t tid;
    pthread_attr_t attr;
    void *status;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    ret = pthread_create(&tid, &attr, client_thread, (void *)0L);
    check(ret == 0, "Failed to create client thread");

    ret = pthread_join(tid, &status);
    check(ret == 0, "Failed to join client thread");
    check((long)status == 0, "Client thread failed");

    pthread_attr_destroy(&attr);
    return 0;

 error:
    pthread_attr_destroy(&attr);
    return -1;
}
