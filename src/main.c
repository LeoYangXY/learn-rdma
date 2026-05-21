/*
 * main.c - RDMA Echo 程序入口
 *
 * 执行流程:
 *   1. parse_config_file()     → 读配置：我是谁？对端是谁？
 *   2. init_env()              → 打开日志文件
 *   3. setup_ib()              → ★ 创建 RDMA 资源 + 建立连接
 *   4. run_server/client()     → ★ Echo 数据传输
 *   5. close_ib_connection()   → 销毁资源
 *
 * 用法: ./rdma-tutorial <config_file> <port> [server|client]
 */

#include <sys/utsname.h>
#include "common.h"

/* 全局变量 */
FILE *log_fp = NULL;
struct ConfigInfo config_info;

/* ============================================================================
 * 配置文件解析
 * ============================================================================ */

static void clean_up_line(char *line)
{
    char *i = line, *j = line;
    while (*j != 0) {
        *i = *j;
        j += 1;
        if (*i != ' ' && *i != '\t' && *i != '\r' && *i != '\n')
            i += 1;
    }
    *i = 0;
}

static int parse_node_list(char *line, char ***node_list)
{
    int start = 0, end = 0, num_nodes = 0;
    char *i = line;
    char node_name_prefix[128] = {'\0'};
    char *j = node_name_prefix;

    while (*i != '.') {
        if ((*i >= '0') && (*i <= '9'))
            start = start * 10 + *i - '0';
        else { *j = *i; j += 1; }
        i += 1;
    }
    i += 2;  /* skip ".." */

    while (*i != 0) {
        if ((*i >= '0') && (*i <= '9'))
            end = end * 10 + *i - '0';
        i += 1;
    }

    num_nodes = end - start + 1;
    check(num_nodes > 0, "Invalid number of nodes: %d", num_nodes);

    *node_list = (char **)calloc(num_nodes, sizeof(char *));
    check(*node_list != NULL, "Failed to allocate node_list");

    int k, node_ind = start;
    for (k = 0; k < num_nodes; k++) {
        (*node_list)[k] = (char *)calloc(128, sizeof(char));
        check((*node_list)[k] != NULL, "Failed to allocate node_list[%d]", k);
        sprintf((*node_list)[k], "%s%d", node_name_prefix, node_ind);
        node_ind += 1;
    }
    return num_nodes;

 error:
    return -1;
}

int parse_config_file(char *fname)
{
    int ret = 0;
    FILE *fp = NULL;
    char line[128] = {'\0'};
    int attr = 0;

    enum { ATTR_SERVERS = 1, ATTR_CLIENTS, ATTR_MSG_SIZE, ATTR_NUM_CONCURR_MSGS };

    fp = fopen(fname, "r");
    check(fp != NULL, "Failed to open config file %s", fname);

    while (fgets(line, 128, fp) != NULL) {
        if (strstr(line, "#")) continue;
        clean_up_line(line);

        if (strstr(line, "servers:"))          { attr = ATTR_SERVERS; continue; }
        else if (strstr(line, "clients:"))     { attr = ATTR_CLIENTS; continue; }
        else if (strstr(line, "msg_size:"))    { attr = ATTR_MSG_SIZE; continue; }
        else if (strstr(line, "num_concurr_msgs:")) { attr = ATTR_NUM_CONCURR_MSGS; continue; }

        if (attr == ATTR_SERVERS) {
            ret = parse_node_list(line, &config_info.servers);
            check(ret > 0, "Failed to get server list");
            config_info.num_servers = ret;
        } else if (attr == ATTR_CLIENTS) {
            ret = parse_node_list(line, &config_info.clients);
            check(ret > 0, "Failed to get client list");
            config_info.num_clients = ret;
        } else if (attr == ATTR_MSG_SIZE) {
            config_info.msg_size = atoi(line);
        } else if (attr == ATTR_NUM_CONCURR_MSGS) {
            config_info.num_concurr_msgs = atoi(line);
        }
        attr = 0;
    }

    /* 默认角色（会被命令行覆盖） */
    config_info.rank = 0;
    config_info.is_server = true;

    fclose(fp);
    return 0;

 error:
    if (fp) fclose(fp);
    return -1;
}

void print_config_info()
{
    log(LOG_SUB_HEADER, "Configuration");
    log("is_server        = %s", config_info.is_server ? "true" : "false");
    log("rank             = %d", config_info.rank);
    log("msg_size         = %d", config_info.msg_size);
    log("num_concurr_msgs = %d", config_info.num_concurr_msgs);
    log("sock_port        = %s", config_info.sock_port);
    log(LOG_SUB_HEADER, "End of Configuration");
}

/* ============================================================================
 * 入口
 * ============================================================================ */

static int init_env()
{
    char fname[64] = {'\0'};
    if (config_info.is_server)
        sprintf(fname, "server[%d].log", config_info.rank);
    else
        sprintf(fname, "client[%d].log", config_info.rank);

    log_fp = fopen(fname, "w");
    check(log_fp != NULL, "Failed to open log file");
    log(LOG_HEADER, "IB Echo Server");
    print_config_info();
    return 0;
 error:
    return -1;
}

static void destroy_env()
{
    log(LOG_HEADER, "Run Finished");
    if (log_fp) fclose(log_fp);
}


/*
 * 命令行参数:
 *   ./build/rdma-tutorial  local.config  56789  client
 *           argv[0]          argv[1]    argv[2]  argv[3]
 *   argc = 4（包括程序名本身）
 */
int main(int argc, char *argv[])
{
    
    parse_config_file(argv[1]);
    config_info.sock_port = argv[2];


    if (strcmp(argv[3], "server") == 0){ 
        config_info.is_server = true;  
        config_info.rank = 0; 
    }else if (strcmp(argv[3], "client") == 0){ 
        config_info.is_server = false; 
        config_info.rank = 0; 
    }

    init_env();//打开日志文件
    setup_ib();//初始化

    //具体的工作
    if (config_info.is_server)
        run_server();
    else
        run_client();

    close_ib_connection();
    destroy_env();
    return 0;
}
