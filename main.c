/*
 * main.c - RDMA Echo 程序入口
 *
 * ============================================================================
 * 【程序整体执行流程】
 * ============================================================================
 *
 *  main()
 *    │
 *    ├── 1. parse_config_file()    解析配置文件，确定角色(Server/Client)和参数
 *    │
 *    ├── 2. init_env()             初始化日志文件
 *    │
 *    ├── 3. setup_ib()             创建 RDMA 资源 + 建立连接 (核心)
 *    │       ├── 打开 IB 设备, 分配 PD, 注册 MR, 创建 CQ/SRQ/QP
 *    │       └── 通过 TCP 交换 QP 信息, 将 QP 转为 RTS 状态
 *    │
 *    ├── 4. run_server/client()    执行 Echo 工作负载
 *    │       ├── 预投递 Recv WR
 *    │       ├── 主循环: 收发消息 + 轮询 CQ
 *    │       └── 统计吞吐率
 *    │
 *    ├── 5. close_ib_connection()  销毁所有 RDMA 资源
 *    │
 *    └── 6. destroy_env()          关闭日志文件
 *
 * 用法: ./rdma-tutorial <config_file> <sock_port>
 *   config_file: 配置文件路径 (参见 sample.config)
 *   sock_port:   TCP 辅助端口号 (用于 QP 信息交换)
 *
 * ============================================================================
 */

#include <stdio.h>

#include "debug.h"
#include "config.h"
#include "ib.h"
#include "setup_ib.h"
#include "client.h"
#include "server.h"

/* 全局日志文件指针，DEBUG 模式下同时输出到 stderr 和文件 */
FILE	*log_fp	     = NULL;

int	init_env    ();
void	destroy_env ();

int main (int argc, char *argv[])
{
    int	ret = 0;

    /* 参数校验 */
    if (argc != 3) {
	printf ("Usage: %s config_file sock_port\n", argv[0]);
	return 0;
    }    

    /* 步骤1: 解析配置文件
     * 从配置文件中读取服务器/客户端列表、消息大小、并发数等参数。
     * 同时根据本机 hostname 自动判断当前节点是 Server 还是 Client。 */
    ret = parse_config_file (argv[1]);
    check (ret == 0, "Failed to parse config file");
    config_info.sock_port = argv[2];    /* TCP 辅助端口号 */

    /* 步骤2: 初始化环境 (日志文件) */
    ret = init_env ();
    check (ret == 0, "Failed to init env");

    /* 步骤3: 创建所有 RDMA 资源并建立连接
     * 这是整个程序最关键的初始化步骤，完成后 QP 进入 RTS 状态 */
    ret = setup_ib ();
    check (ret == 0, "Failed to setup IB");

    /* 步骤4: 根据角色执行不同的工作负载 */
    if (config_info.is_server) {
        ret = run_server ();    /* Echo 响应者 */
    } else {
        ret = run_client ();    /* Echo 发起者 */
    }
    check (ret == 0, "Failed to run workload");

 error:
    /* 步骤5+6: 清理资源 (无论成功失败都执行) */
    close_ib_connection ();
    destroy_env         ();
    return ret;
}    

/*
 * init_env - 初始化运行环境
 * 根据角色 (Server/Client) 和 rank 创建对应的日志文件。
 * 例如: server[0].log, client[1].log
 */
int init_env ()
{
    char fname[64] = {'\0'};

    if (config_info.is_server) {
	sprintf (fname, "server[%d].log", config_info.rank);
    } else {
	sprintf (fname, "client[%d].log", config_info.rank);
    }
    log_fp = fopen (fname, "w");
    check (log_fp != NULL, "Failed to open log file");

    log (LOG_HEADER, "IB Echo Server");
    print_config_info ();

    return 0;
 error:
    return -1;
}

/* destroy_env - 清理运行环境，关闭日志文件 */
void destroy_env ()
{
    log (LOG_HEADER, "Run Finished");
    if (log_fp != NULL) {
        fclose (log_fp);
    }
}
