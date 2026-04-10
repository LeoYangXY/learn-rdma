/*
 * server.h - RDMA Echo Server 头文件
 *
 * Server 端的主要职责:
 *   1. 预投递 Recv WR 到 SRQ
 *   2. 发送 START 信号通知 Client 开始
 *   3. 循环: 收到消息 → 原样回送 (Echo) → 补充 Recv WR
 *   4. 达到目标次数后发送 STOP 信号
 */

#ifndef SERVER_H_
#define SERVER_H_

int run_server ();

#endif /* server.h */
