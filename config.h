/*
 * config.h - 配置管理头文件
 *
 * 定义了程序运行所需的配置参数结构体和配置文件解析接口。
 * 配置文件格式参见 sample.config。
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>
#include <inttypes.h>

/* 配置文件属性类型枚举，用于状态机解析 */
enum ConfigFileAttr {
    ATTR_SERVERS = 1,       /* 正在解析 servers 列表 */
    ATTR_CLIENTS,           /* 正在解析 clients 列表 */
    ATTR_MSG_SIZE,          /* 正在解析消息大小 */
    ATTR_NUM_CONCURR_MSGS,  /* 正在解析并发消息数 */
};

/*
 * ConfigInfo - 全局配置信息
 *
 * 包含 RDMA Echo 测试所需的所有参数:
 *   - 节点拓扑: 哪些节点是 Server，哪些是 Client
 *   - 当前身份: 本节点是 Server 还是 Client，rank 是多少
 *   - 测试参数: 消息大小、并发度
 *   - 辅助参数: TCP 端口号
 */
struct ConfigInfo {
    int  num_servers;           /* Server 节点数量 */
    int  num_clients;           /* Client 节点数量 */
    char **servers;             /* Server 主机名列表 */
    char **clients;             /* Client 主机名列表 */
    
    bool is_server;             /* 当前节点是否为 Server (通过 hostname 匹配判断) */
    int  rank;                  /* 当前节点在 Server/Client 列表中的编号 (从0开始) */

    int  msg_size;              /* 每条 Echo 消息的大小 (字节) */
    int  num_concurr_msgs;      /* 每个 QP 的并发消息数 (流水线深度) */

    char *sock_port;            /* TCP 辅助通道的端口号 (命令行参数传入) */
}__attribute__((aligned(64)));  /* 64字节对齐，避免 false sharing */

/* 全局配置实例 */
extern struct ConfigInfo config_info;

/* 解析配置文件，填充 config_info */
int  parse_config_file   (char *fname);

/* 释放配置信息中的动态内存 */
void destroy_config_info ();

/* 打印当前配置信息到日志 */
void print_config_info ();

#endif /* CONFIG_H_*/
