/*
 * sock.h - TCP Socket 辅助通道头文件
 *
 * 【在 RDMA 通信中的作用】
 * TCP Socket 在本项目中充当"带外通道"(out-of-band channel)，
 * 用于 RDMA 连接建立前的信息交换。
 *
 * RDMA 连接建立需要双方交换 QP 信息 (LID + QP号)，
 * 但此时 RDMA 连接还不存在，无法用 RDMA 来传输。
 * 所以借助传统的 TCP Socket 来完成这个"引导"过程。
 *
 * 一旦 RDMA 连接建立完毕 (QP 进入 RTS 状态)，
 * TCP 连接就会被关闭，后续所有数据传输都通过 RDMA 进行。
 */

#ifndef SOCK_H_
#define SOCK_H_

#include <inttypes.h>

#include "ib.h"

/* TCP 同步消息: 用于确认双方都已完成 QP 状态转换 */
#define SOCK_SYNC_MSG     "sync"

/* 可靠读: 确保读满 len 字节才返回 (处理了部分读和信号中断) */
ssize_t sock_read (int sock_fd, void *buffer, size_t len);

/* 可靠写: 确保写满 len 字节才返回 (处理了部分写和信号中断) */
ssize_t sock_write (int sock_fd, void *buffer, size_t len);

/* 创建 TCP 服务端 Socket 并绑定端口 (Server 端用) */
int sock_create_bind (char *port);

/* 创建 TCP 客户端 Socket 并连接到 Server (Client 端用) */
int sock_create_connect (char *server_name, char *port);

/* 通过 TCP 发送本地 QP 信息 (转为网络字节序后发送) */
int sock_set_qp_info(int sock_fd, struct QPInfo *qp_info);

/* 通过 TCP 接收对端 QP 信息 (接收后转为主机字节序) */
int sock_get_qp_info(int sock_fd, struct QPInfo *qp_info);

#endif /* SOCK_H_ */
