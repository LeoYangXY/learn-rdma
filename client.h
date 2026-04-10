/*
 * client.h - RDMA Echo Client 头文件
 *
 * Client 端的主要职责:
 *   1. 预投递 Recv WR 到 SRQ
 *   2. 等待 Server 的 START 信号
 *   3. 主动发起第一批 Echo 消息 (post_send)
 *   4. 循环: 收到回送 → 再次发送 → 补充 Recv WR
 *   5. 收到 STOP 信号后停止
 */

#ifndef CLIENT_H_
#define CLIENT_H_

int run_client ();

#endif /* client.h */
