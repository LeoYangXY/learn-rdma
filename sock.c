/*
 * sock.c - TCP Socket 辅助通道实现
 *
 * 本文件实现了 RDMA 连接建立所需的 TCP 辅助功能:
 *   1. 可靠的 TCP 读写 (处理部分读写和信号中断)
 *   2. TCP 服务端/客户端 Socket 创建
 *   3. QP 信息的序列化发送/反序列化接收
 *
 * 【在整体链路中的位置】
 *   main → setup_ib → connect_qp_server/client → 本文件的函数
 *   TCP 通道在 QP 连接建立完成后关闭，不参与后续 RDMA 数据传输。
 */

#define _GNU_SOURCE
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

#include "debug.h"
#include "sock.h"

/*
 * sock_read - 可靠地从 TCP socket 读取恰好 len 字节
 *
 * 标准 read() 系统调用可能只返回部分数据（TCP 是流式协议），
 * 本函数循环读取直到读满 len 字节或遇到 EOF/错误。
 *
 * 处理了 EINTR (信号中断): 被信号中断时自动重试，不算错误。
 */
ssize_t sock_read (int sock_fd, void *buffer, size_t len)
{
    ssize_t nr, tot_read;
    char *buf = buffer; /* 避免对 void 指针做算术运算 */
    tot_read = 0;

    while (len !=0 && (nr = read(sock_fd, buf, len)) != 0) {
        if (nr < 0) {
            if (errno == EINTR) {
                continue;       /* 被信号中断，重试 */
            } else {
                return -1;      /* 其他错误 */
            }
        }
        len -= nr;              /* 剩余要读的字节数 */
        buf += nr;              /* 移动缓冲区指针 */
        tot_read += nr;         /* 累计已读字节数 */
    }

    return tot_read;
}

/*
 * sock_write - 可靠地向 TCP socket 写入恰好 len 字节
 *
 * 与 sock_read 类似，处理了部分写和 EINTR。
 */
ssize_t sock_write (int sock_fd, void *buffer, size_t len)
{
    ssize_t nw, tot_written;
    const char *buf = buffer;

    for (tot_written = 0; tot_written < len; ) {
        nw = write(sock_fd, buf, len-tot_written);

        if (nw <= 0) {
            if (nw == -1 && errno == EINTR) {
                continue;       /* 被信号中断，重试 */
            } else {
                return -1;
            }
        }

        tot_written += nw;
        buf += nw;
    }
    return tot_written;
}

/*
 * sock_create_bind - 创建 TCP 服务端 Socket 并绑定到指定端口
 *
 * 使用 getaddrinfo 实现协议无关 (支持 IPv4/IPv6)。
 * AI_PASSIVE 标志表示用于 bind (监听)，不指定具体 IP。
 * 遍历所有可用地址尝试 bind，成功即返回。
 *
 * 返回: 绑定成功的 socket fd (还需要调用 listen)
 */
int sock_create_bind (char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sock_fd = -1, ret = 0;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;    /* TCP */
    hints.ai_family = AF_UNSPEC;        /* IPv4 或 IPv6 均可 */
    hints.ai_flags = AI_PASSIVE;        /* 用于 bind，监听所有接口 */

    ret = getaddrinfo(NULL, port, &hints, &result);
    check(ret==0, "getaddrinfo error.");

    /* 遍历所有可用地址，尝试 socket + bind */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) {
            continue;
        }

        ret = bind(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            break;      /* bind 成功 */
        }

        close(sock_fd);
        sock_fd = -1;
    }

    check(rp != NULL, "creating socket.");

    freeaddrinfo(result);
    return sock_fd;

 error:
    if (result) {
        freeaddrinfo(result);
    }
    if (sock_fd > 0) {
        close(sock_fd);
    }
    return -1;
}

/*
 * sock_create_connect - 创建 TCP 客户端 Socket 并连接到指定 Server
 *
 * 与 sock_create_bind 类似，但使用 connect 而非 bind。
 * 遍历所有可用地址尝试连接，成功即返回。
 *
 * 返回: 已连接的 socket fd
 */
int sock_create_connect (char *server_name, char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sock_fd = -1, ret = 0;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;    /* TCP */
    hints.ai_family = AF_UNSPEC;        /* IPv4 或 IPv6 均可 */

    ret = getaddrinfo(server_name, port, &hints, &result);
    check(ret==0, "[ERROR] %s", gai_strerror(ret));

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd == -1) {
            continue;
        }

        ret = connect(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            break;      /* 连接成功 */
        }

        close(sock_fd);
        sock_fd = -1;
    }

    check(rp!=NULL, "could not connect.");

    freeaddrinfo(result);
    return sock_fd;

 error:
    if (result) {
        freeaddrinfo(result);
    }
    if (sock_fd != -1) {
        close(sock_fd);
    }
    return -1;
}

/*
 * sock_set_qp_info - 通过 TCP 发送本地 QP 信息给对端
 *
 * 发送前将所有字段转为网络字节序 (大端)，确保跨平台兼容：
 *   lid:    16位, htons
 *   qp_num: 32位, htonl
 *   rank:   32位, htonl
 *
 * 使用临时变量 tmp_qp_info 避免修改原始数据。
 */
int sock_set_qp_info(int sock_fd, struct QPInfo *qp_info)
{
    int n;
    struct QPInfo tmp_qp_info;

    /* 转为网络字节序 */
    tmp_qp_info.lid       = htons(qp_info->lid);
    tmp_qp_info.qp_num    = htonl(qp_info->qp_num);
    tmp_qp_info.rank      = htonl(qp_info->rank);

    /* 通过 TCP 发送整个结构体 (packed, 无填充字节) */
    n = sock_write(sock_fd, (char *)&tmp_qp_info, sizeof(struct QPInfo));
    check(n==sizeof(struct QPInfo), "write qp_info to socket.");

    return 0;

 error:
    return -1;
}

/*
 * sock_get_qp_info - 通过 TCP 接收对端的 QP 信息
 *
 * 接收后将所有字段从网络字节序转为主机字节序。
 * 接收到的信息 (lid + qp_num) 后续用于 modify_qp_to_rts 中的 RTR 阶段，
 * 配置数据包的路由信息。
 */
int sock_get_qp_info(int sock_fd, struct QPInfo *qp_info)
{
    int n;
    struct QPInfo  tmp_qp_info;

    /* 从 TCP 接收整个结构体 */
    n = sock_read(sock_fd, (char *)&tmp_qp_info, sizeof(struct QPInfo));
    check(n==sizeof(struct QPInfo), "read qp_info from socket.");

    /* 转为主机字节序 */
    qp_info->lid       = ntohs(tmp_qp_info.lid);
    qp_info->qp_num    = ntohl(tmp_qp_info.qp_num);
    qp_info->rank      = ntohl(tmp_qp_info.rank);
    
    return 0;

 error:
    return -1;
}
