/*
 * tcp_helper.c - TCP 辅助通道
 *
 * 【作用】
 * RDMA 连接建立前，双方需要交换 QP 信息（lid + qp_number + gid）。
 * 但此时 RDMA 连接还不存在，无法用 RDMA 传输这些信息。
 * 所以先用传统的 TCP 来完成这个"引导"过程。
 * RDMA 连接建立后 TCP 就关闭了，后续全走 RDMA。
 */

#define _GNU_SOURCE
#include <sys/socket.h>
#include <netdb.h>
#include "common.h"

/* --------------------------------------------------------------------------
 * sock_read / sock_write - 可靠的 TCP 读写
 *
 * TCP 是流式协议，一次 read/write 可能只传输了部分数据。
 * 这两个函数循环操作，确保恰好传输 len 字节。
 * -------------------------------------------------------------------------- */

ssize_t sock_read(int sock_fd, void *buffer, size_t len)
{
    ssize_t nr, tot_read;
    char *buf = buffer;
    tot_read = 0;

    while (len != 0 && (nr = read(sock_fd, buf, len)) != 0) {
        if (nr < 0) {
            if (errno == EINTR) continue;  /* 被信号中断，重试 */
            else return -1;
        }
        len -= nr;
        buf += nr;
        tot_read += nr;
    }
    return tot_read;
}

ssize_t sock_write(int sock_fd, void *buffer, size_t len)
{
    ssize_t nw, tot_written;
    const char *buf = buffer;

    for (tot_written = 0; tot_written < (ssize_t)len; ) {
        nw = write(sock_fd, buf, len - tot_written);
        if (nw <= 0) {
            if (nw == -1 && errno == EINTR) continue;
            else return -1;
        }
        tot_written += nw;
        buf += nw;
    }
    return tot_written;
}

/* --------------------------------------------------------------------------
 * sock_create_bind - 创建 TCP 服务端 Socket
 *
 * Server 端用。绑定端口后等待 Client 连接。
 * 使用 SO_REUSEADDR 避免 "Address already in use" 错误。
 * -------------------------------------------------------------------------- */

int sock_create_bind(char *port)
{
    struct addrinfo hints, *result, *rp;
    int sock_fd = -1, ret = 0;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;   /* 监听所有接口 */

    ret = getaddrinfo(NULL, port, &hints, &result);
    check(ret == 0, "getaddrinfo error.");

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) continue;

        int optval = 1;
        setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        ret = bind(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) break;

        close(sock_fd);
        sock_fd = -1;
    }
    check(rp != NULL, "creating socket.");

    freeaddrinfo(result);
    return sock_fd;

 error:
    if (result) freeaddrinfo(result);
    if (sock_fd > 0) close(sock_fd);
    return -1;
}

/* --------------------------------------------------------------------------
 * sock_create_connect - 创建 TCP 客户端 Socket 并连接
 *
 * Client 端用。连接到 Server 的 IP:port。
 * -------------------------------------------------------------------------- */

int sock_create_connect(char *server_name, char *port)
{
    struct addrinfo hints, *result, *rp;
    int sock_fd = -1, ret = 0;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    ret = getaddrinfo(server_name, port, &hints, &result);
    check(ret == 0, "[ERROR] %s", gai_strerror(ret));

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd == -1) continue;

        ret = connect(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) break;

        close(sock_fd);
        sock_fd = -1;
    }
    check(rp != NULL, "could not connect.");

    freeaddrinfo(result);
    return sock_fd;

 error:
    if (result) freeaddrinfo(result);
    if (sock_fd != -1) close(sock_fd);
    return -1;
}

/* --------------------------------------------------------------------------
 * sock_set/get_qp_info - 交换 QP 连接信息
 *
 * 发送时：转为网络字节序（大端），确保跨平台兼容
 * 接收时：转回主机字节序
 * GID 本身是 128-bit 大端，直接 memcpy
 * -------------------------------------------------------------------------- */

int sock_set_qp_info(int sock_fd, struct QPInfo *qp_info)
{
    int n;
    struct QPInfo tmp;

    tmp.lid    = htons(qp_info->lid);
    tmp.qp_number = htonl(qp_info->qp_number);
    tmp.rank   = htonl(qp_info->rank);
    memcpy(&tmp.gid, &qp_info->gid, sizeof(union ibv_gid));

    n = sock_write(sock_fd, (char *)&tmp, sizeof(struct QPInfo));
    check(n == sizeof(struct QPInfo), "write qp_info to socket.");
    return 0;

 error:
    return -1;
}

int sock_get_qp_info(int sock_fd, struct QPInfo *qp_info)
{
    int n;
    struct QPInfo tmp;

    n = sock_read(sock_fd, (char *)&tmp, sizeof(struct QPInfo));
    check(n == sizeof(struct QPInfo), "read qp_info from socket.");

    qp_info->lid    = ntohs(tmp.lid);
    qp_info->qp_number = ntohl(tmp.qp_number);
    qp_info->rank   = ntohl(tmp.rank);
    memcpy(&qp_info->gid, &tmp.gid, sizeof(union ibv_gid));
    return 0;

 error:
    return -1;
}
