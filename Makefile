# ============================================================================
# Makefile - RDMA Echo 程序构建文件
# ============================================================================
#
# 构建命令:
#   make        - 编译 Release 版本 (开启优化, 无调试信息)
#   make debug  - 编译 Debug 版本 (开启调试信息 + DEBUG 宏)
#   make clean  - 清除编译产物
#
# 链接库说明:
#   -libverbs   : libibverbs, RDMA Verbs API 核心库
#   -pthread    : POSIX 线程库 (server/client 工作线程)
#   -lrdmacm    : RDMA Communication Manager 库 (本项目未直接使用其API)
#
# ============================================================================

CC=gcc
CFLAGS=-Wall -Werror -O2
INCLUDES=
LDFLAGS=-libverbs
LIBS=-pthread -lrdmacm

SRCS=main.c client.c config.c ib.c server.c setup_ib.c sock.c
OBJS=$(SRCS:.c=.o)
PROG=rdma-tutorial

all: $(PROG)

# Debug 版本: 关闭优化, 开启调试符号(-g), 定义 DEBUG 宏
# DEBUG 宏会启用 debug.h 中的 debug() 和详细 log() 输出
debug: CFLAGS=-Wall -Werror -g -DDEBUG
debug: $(PROG)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	$(RM) *.o *~ $(PROG)
