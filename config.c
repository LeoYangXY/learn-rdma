/*
 * config.c - 配置文件解析实现
 *
 * 负责解析 sample.config 格式的配置文件，确定:
 *   1. Server/Client 节点列表
 *   2. 消息大小和并发度
 *   3. 当前节点的角色 (Server/Client) 和 rank
 *
 * 配置文件示例:
 *   servers:
 *       server1 .. server2     (表示 server1, server2 两个节点)
 *   clients:
 *       client1 .. client2
 *   num_concurr_msgs:
 *       64                     (每个 QP 同时有 64 条消息在飞行)
 *   msg_size:
 *       8                      (每条消息 8 字节)
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/utsname.h>

#include "debug.h"
#include "config.h"

/* 全局配置实例 */
struct ConfigInfo config_info;

/* 清除行中的空格、制表符和换行符 */
void clean_up_line (char *line)
{
    char *i = line;
    char *j = line;

    while (*j != 0) {
        *i = *j;
        j += 1;
        if (*i != ' ' && *i != '\t' && *i != '\r' && *i != '\n') {
            i += 1;
        }
    }
    *i = 0;
}

/*
 * parse_node_list - 解析节点列表
 *
 * 输入格式: "server1..server2" (清除空格后)
 * 解析逻辑:
 *   1) ".." 前的部分: 提取前缀名 + 起始编号 (如 "server" + 1)
 *   2) ".." 后的部分: 提取结束编号 (如 2)
 *   3) 生成节点名列表: server1, server2
 *
 * 返回: 节点数量
 */
int parse_node_list (char *line, char ***node_list)
{
    int start = 0, end = 0, num_nodes=0;
    char *i = line;
    char node_name_prefix[128] = {'\0'};
    char *j = node_name_prefix;

    /* 解析 ".." 前的部分: 提取字母前缀和起始数字 */
    while (*i != '.') {
        if ((*i >= '0') && (*i <= '9')) {
            start = start * 10 + *i - '0';
        } else {
            *j = *i;
            j += 1;
        }
        i += 1;
    }

    /* 跳过 ".." */
    i += 2;

    /* 解析 ".." 后的部分: 提取结束数字 */
    while (*i != 0) {
        if ((*i >= '0') && (*i <= '9')) {
            end = end * 10 + *i - '0';
        }
        i += 1;
    }

    num_nodes = end - start + 1;
    check (num_nodes > 0, "Invaild number of nodes: %d", num_nodes);

    /* 分配节点名数组 */
    *node_list = (char **) calloc (num_nodes, sizeof(char *));
    if (*node_list == NULL){
        printf ("Failed to allocate node_list.\n");
        return 0;
    }

    /* 生成每个节点的主机名: 前缀 + 编号 */
    int k = 0, node_ind = start;
    
    for (k = 0; k < num_nodes; k++) {
        (*node_list)[k] = (char *) calloc (128, sizeof(char));
        check ((*node_list)[k] != NULL,
               "Failed to allocate node_list[%d]", k);

        if (strstr(node_name_prefix, "mnemosyne")) {
            sprintf ((*node_list)[k], "mnemosyne%02d", node_ind);
        } else {
            sprintf ((*node_list)[k], "saguaro%d", node_ind);
        }

        node_ind += 1;
    }

    return num_nodes;

 error:
    return -1;
}

/*
 * get_rank - 根据 hostname 确定当前节点的角色和 rank
 *
 * 通过 uname() 获取本机 hostname，然后:
 *   1) 在 Server 列表中查找: 找到则 is_server=true, rank=索引
 *   2) 在 Client 列表中查找: 找到则 is_server=false, rank=索引
 *   3) 都没找到: 报错退出
 *
 * 如果同一个 hostname 同时出现在 Server 和 Client 列表中，也报错。
 */
int get_rank ()
{
    int			ret	    = 0;
    uint32_t		i	    = 0;
    uint32_t		num_servers = config_info.num_servers;
    uint32_t		num_clients = config_info.num_clients;
    struct utsname	utsname_buf;
    char		hostname[64];

    /* 获取本机 hostname */
    ret = uname (&utsname_buf);
    check (ret == 0, "Failed to call uname");

    strncpy (hostname, utsname_buf.nodename, sizeof(hostname));

    config_info.rank = -1;

    /* 在 Server 列表中查找 */
    for (i = 0; i < num_servers; i++) {
        if (strstr(hostname, config_info.servers[i])) {
            config_info.rank      = i;
            config_info.is_server = true;
            break;
        }
    }

    /* 在 Client 列表中查找 */
    for (i = 0; i < num_clients; i++) {
        if (strstr(hostname, config_info.clients[i])) {
            if (config_info.rank == -1) {
                config_info.rank      = i;
                config_info.is_server = false;
                break;
            } else {
                check (0, "node (%s) listed as both server and client", hostname);
            }
        }
    }

    check (config_info.rank >= 0, "Failed to get rank for node: %s", hostname);

    return 0;
 error:
    return -1;
}

/*
 * parse_config_file - 解析配置文件
 *
 * 使用简单的状态机解析:
 *   1) 读取一行
 *   2) 跳过注释行 (以 # 开头)
 *   3) 识别属性标签 (servers: / clients: / msg_size: / num_concurr_msgs:)
 *   4) 下一行读取对应的值
 *   5) 最后调用 get_rank() 确定本节点身份
 */
int parse_config_file (char *fname)
{
    int ret = 0;
    FILE *fp = NULL;
    char line[128] = {'\0'};
    int  attr = 0;      /* 当前正在解析的属性类型 */

    fp = fopen (fname, "r");
    check (fp != NULL, "Failed to open config file %s", fname);

    while (fgets(line, 128, fp) != NULL) {
        /* 跳过注释行 */
        if (strstr(line, "#") != NULL) {
            continue;
        }

        /* 移除空白字符 */
        clean_up_line (line);

        /* 检测属性标签 */
	if (strstr (line, "servers:")) {
            attr = ATTR_SERVERS;
            continue;
        } else if (strstr (line, "clients:")) {
            attr = ATTR_CLIENTS;
            continue;
        } else if (strstr (line, "msg_size:")) {
            attr = ATTR_MSG_SIZE;
            continue;
        } else if (strstr (line, "num_concurr_msgs:")) {
            attr = ATTR_NUM_CONCURR_MSGS;
            continue;
        }

        /* 根据当前属性类型解析值 */
	if (attr == ATTR_SERVERS) {
            ret = parse_node_list (line, &config_info.servers);
            check (ret > 0, "Failed to get server list");
            config_info.num_servers = ret;
        } else if (attr == ATTR_CLIENTS) {
            ret = parse_node_list (line, &config_info.clients);
            check (ret > 0, "Failed to get client list");
            config_info.num_clients = ret;
        } else if (attr == ATTR_MSG_SIZE) {
            config_info.msg_size = atoi(line);
            check (config_info.msg_size > 0,
                   "Invalid Value: msg_size = %d",
                   config_info.msg_size);
        } else if (attr == ATTR_NUM_CONCURR_MSGS) {
            config_info.num_concurr_msgs = atoi(line);
            check (config_info.num_concurr_msgs > 0,
                   "Invalid Value: num_concurr_msgs = %d",
                   config_info.num_concurr_msgs);
        }

        attr = 0;  /* 重置状态 */
    }

    /* 根据 hostname 判断当前节点角色和 rank */
    ret = get_rank ();
    check (ret == 0, "Failed to get rank");

    fclose (fp);

    return 0;

 error:
    if (fp != NULL) {
        fclose (fp);
    }
    return -1;
}

/* 释放配置信息中动态分配的内存 */
void destroy_config_info ()
{
    int num_servers = config_info.num_servers;
    int num_clients = config_info.num_clients;
    int i;

    if (config_info.servers != NULL) {
        for (i = 0; i < num_servers; i++) {
            if (config_info.servers[i] != NULL) {
                free (config_info.servers[i]);
            }
        }
        free (config_info.servers);
    }

    if (config_info.clients != NULL) {
        for (i = 0; i < num_clients; i++) {
            if (config_info.clients[i] != NULL) {
                free (config_info.clients[i]);
            }
        }
        free (config_info.clients);
    }
}

/* 打印配置信息到日志 */
void print_config_info ()
{
    log (LOG_SUB_HEADER, "Configuraion");

    if (config_info.is_server) {
	log ("is_server                 = %s", "true");
    } else {
	log ("is_server                 = %s", "false");
    }
    log ("rank                      = %d", config_info.rank);
    log ("msg_size                  = %d", config_info.msg_size);
    log ("num_concurr_msgs          = %d", config_info.num_concurr_msgs);
    log ("sock_port                 = %s", config_info.sock_port);
    
    log (LOG_SUB_HEADER, "End of Configuraion");
}
