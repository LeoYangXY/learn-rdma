#ifndef COMMON_H_
#define COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

/* ============================================================================
 * 错误处理 & 日志
 *
 * check(条件, "错误信息"):  条件为假 → 打印错误 → goto error
 * log("格式", ...):        输出到日志文件（Debug 模式也输出到 stderr）
 * debug("格式", ...):      仅 Debug 模式有输出
 * ============================================================================ */

#define LOG_HEADER     "\n================ %s ================\n"
#define LOG_SUB_HEADER "\n************ %s ************\n"

extern FILE *log_fp;

#define clean_errno() (errno == 0 ? "None" : strerror(errno))
#define log_err(M, ...) fprintf(stderr, "[ERROR] (%s:%d:%s: errno: %s) " M "\n",\
                __FILE__, __LINE__, __func__, clean_errno(), ##__VA_ARGS__)
#define log_file(M, ...) {fprintf(log_fp, "" M "\n", ##__VA_ARGS__);fflush(log_fp);}
#define check(A, M, ...) if(!(A)) {log_err(M, ##__VA_ARGS__); errno=0; goto error;}

#ifdef DEBUG
#define debug(M, ...) fprintf(stderr, "[DEBUG] " M "\n", ##__VA_ARGS__)
#define log(M, ...) {fprintf(stderr, "" M "\n", ##__VA_ARGS__); log_file(M, ##__VA_ARGS__);}
#else
#define debug(M, ...)
#define log(M, ...) {log_file(M, ##__VA_ARGS__);}
#endif

/* ============================================================================
 * RDMA 常量
 * ============================================================================ */

#define IB_MTU          IBV_MTU_1024
#define IB_PORT         1
#define IB_SL           0
#define IB_WR_ID_STOP   0xE000000000000000

#define NUM_WARMING_UP_OPS  500
#define TOT_NUM_OPS         10000

/* ============================================================================
 * 控制消息类型（通过 Send with Immediate 的 imm_data 字段传递）
 * ============================================================================ */

enum MsgType {
    MSG_CTL_START = 100,   /* Server → Client: "开始" */
    MSG_CTL_STOP  = 101,   /* Server → Client: "停止" */
};

/* ============================================================================
 * QPInfo - RDMA 连接建立时通过 TCP 交换的 QP 端点信息
 * ============================================================================ */

struct QPInfo {
    uint16_t      lid;       /* IB 链路层地址。RoCE 下无 IB 交换机，没人看，填 0 */
    uint32_t      qp_number; /* ★ QP 编号，网卡内唯一。对端靠 GID + qp_number 定位到你 */
    uint32_t      rank;      /* 非 RDMA 概念，本项目用来匹配"谁连谁" */
    union ibv_gid gid;       /* ★ 全局地址。RoCE 下 = IP 的 128-bit 映射（如 ::ffff:10.0.0.1） */
} __attribute__((aligned(4)));

/* ============================================================================
 * ConfigInfo - 程序配置
 * ============================================================================ */

struct ConfigInfo {
    int    num_servers;        // 有几个 Server
    int    num_clients;        // 有几个 Client
    char **servers;            // Server 主机名列表 ["server1", "server2"]
    char **clients;            // Client 主机名列表 ["client1"]
    bool   is_server;          // 我是 Server 还是 Client？
    int    rank;               // 我在列表里的编号（第几个）
    int    msg_size;           // 每条 Echo 消息多大（字节）
    int    num_concurr_msgs;   // 并发消息数（流水线深度）
    char  *sock_port;          // TCP 辅助通道端口号
};

extern struct ConfigInfo config_info;

/* ============================================================================
 * IBRes - 所有 RDMA 资源
 *
 * 1.【资源关系图】
 *
 *  IBRes
 *   ├── ctx             打开设备得到的句柄
 *   ├── pd              安全围栏（同 pd 下的 QP 和 MR 才能配合使用）
 *   ├── mr ← ib_buf    一块内存 + 注册到网卡（收发数据都用这块）
 *   ├── cq              完成队列（网卡做完事往这里写"回执" WC）
 *   ├── srq             共享接收队列（替代了每个 QP 自己的 RQ）
 *   └── qp[0], qp[1]...  每个连接一个 QP
 *        ├── SQ ← post_send() 投递发送请求
 *        └── RQ（空壳，被 srq 替代）
 *
 * 2.【数据流】
 *
 *  发送: post_send() → WR 进入 qp[i].SQ → 网卡 DMA 读 ib_buf → 发包
 *  接收: 网卡收到包 → 从 srq 取一个 Recv WR → DMA 写入 ib_buf → WC 进入 cq
 *  收割: ibv_poll_cq(cq) → 拿到 WC → 知道"收到了 / 发完了"
 *
 * 3.【ib_buf 与 mr 的关系】
 *
 *         同一块内存
 *      ┌──────────────┐
 *      │  msg msg msg │
 *      └──────────────┘
 *            ↑
 *   ib_buf ──┘  你的程序用这个指针读写数据
 *            ↑
 *   mr ──────┘  网卡用这个凭证 DMA 读写同一块内存
 *                 mr 里有:
 *                   - lkey: 本地密钥，每次收发都要带
 *                   - 起始地址 + 长度: 网卡知道能 DMA 的范围
 * ============================================================================ */

struct IBRes {
    struct ibv_context       *ctx;       /* 设备句柄，所有操作的根 */
    struct ibv_pd            *pd;        /* 保护域，隔离不同应用的资源 */
    struct ibv_mr            *mr;        /* 内存注册凭证，带 lkey */
    struct ibv_cq            *cq;        /* 完成队列，收割操作结果 */
    struct ibv_qp           **qp;        /* QP 数组，每个连接一个 */
    struct ibv_srq           *srq;       /* 共享接收队列 */
    struct ibv_port_attr      port_attr; /* 端口属性（含 LID） */
    struct ibv_device_attr    dev_attr;  /* 设备能力（最大队列深度等） */
    int     num_qps;                     /* QP 数量 */
    char   *ib_buf;                      /* 数据缓冲区指针 */
    size_t  ib_buf_size;                 /* 缓冲区大小 */
};


extern struct IBRes ib_res;

/* ============================================================================
 * 函数声明
 * ============================================================================ */

/* TCP 辅助 */
#define SOCK_SYNC_MSG "sync"
ssize_t sock_read(int sock_fd, void *buffer, size_t len);
ssize_t sock_write(int sock_fd, void *buffer, size_t len);
int     sock_create_bind(char *port);
int     sock_create_connect(char *server_name, char *port);
int     sock_set_qp_info(int sock_fd, struct QPInfo *qp_info);
int     sock_get_qp_info(int sock_fd, struct QPInfo *qp_info);

/* RDMA 核心操作 */
int modify_qp_to_rts(struct ibv_qp *qp, uint32_t qp_number, uint16_t lid, union ibv_gid *dgid);
int post_send(uint32_t req_size, uint32_t lkey, uint64_t wr_id,
              uint32_t imm_data, struct ibv_qp *qp, char *buf);
int post_srq_recv(uint32_t req_size, uint32_t lkey, uint64_t wr_id,
                  struct ibv_srq *srq, char *buf);

/* 高层接口 */
int  parse_config_file(char *fname);
void print_config_info();
int  setup_ib();
void close_ib_connection();
int  server_side_setup_connection();
int  client_side_setup_connection();
int  run_server();
int  run_client();

#endif /* COMMON_H_ */
