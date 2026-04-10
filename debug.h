/*
 * debug.h - 调试和日志宏定义
 *
 * 来源: http://c.learncodethehardway.org/book/ex20.html
 *
 * 提供统一的错误处理和日志机制:
 *   - check(A, M, ...): 断言检查，失败时打印错误信息并 goto error
 *   - log(M, ...):      日志输出 (DEBUG 模式同时输出到 stderr 和文件)
 *   - debug(M, ...):    调试输出 (仅 DEBUG 模式有效)
 *
 * 【错误处理模式】
 * 本项目采用 "goto error" 模式进行错误处理:
 *   ret = some_func();
 *   check(ret == 0, "some_func failed");  // 失败时自动 goto error
 *   ...
 *   return 0;
 *  error:
 *   cleanup();
 *   return -1;
 *
 * 这种模式的好处:
 *   - 错误处理代码集中在函数末尾的 error 标签后
 *   - 避免了深层嵌套的 if-else
 *   - 确保资源清理代码不被遗漏
 */

#ifndef DEBUG_H_
#define DEBUG_H_

#include <stdio.h>
#include <errno.h>
#include <string.h>

/* 日志格式化头部，用于分隔不同阶段的输出 */
#define LOG_HEADER     "\n================ %s ================\n"
#define LOG_SUB_HEADER "\n************ %s ************\n"

/* 全局日志文件指针 (在 main.c 中定义) */
extern FILE *log_fp;

/* 获取 errno 对应的错误描述字符串 (如果 errno==0 则返回 "None") */
#define clean_errno() (errno == 0 ? "None" : strerror(errno))

/* 错误日志: 输出到 stderr，包含文件名、行号、函数名和 errno */
#define log_err(M, ...) fprintf(stderr, "[ERROR] (%s:%d:%s: errno: %s) " M "\n",\
                __FILE__, __LINE__, __func__, clean_errno(), ##__VA_ARGS__)

/* 警告日志: 输出到 stderr */
#define log_warn(M, ...) fprintf(stderr, "[WARN] (%s:%d:%s errno: %s) " M "\n",\
                 __FILE__, __LINE__, __func__, clean_errno(), ##__VA_ARGS__)

/* 信息日志: 输出到 stderr (不含位置信息) */
#define log_info(M, ...) fprintf(stderr, "" M "\n", ##__VA_ARGS__)

/* 文件日志: 输出到日志文件并立即刷新 */
#define log_file(M, ...) {fprintf(log_fp, "" M "\n", ##__VA_ARGS__);fflush(log_fp);}

/* sentinel: 无条件跳转到 error 标签 (用于"不应到达"的代码路径) */
#define sentinel(M, ...) {log_err(M, ##__VA_ARGS__); errno=0; goto error;}

/*
 * check: 条件检查宏 —— 本项目最核心的错误处理机制
 * 如果条件 A 为假，打印错误信息并跳转到 error 标签。
 * 用法: check(ret == 0, "Failed to do X, ret=%d", ret);
 */
#define check(A, M, ...) if(!(A)) {log_err(M, ##__VA_ARGS__); errno=0; goto error;}

/* DEBUG 模式: 编译时加 -DDEBUG 启用 (Makefile 中 debug target) */
#ifdef DEBUG
/* 详细调试: 包含文件名、行号、函数名 */
#define debug_detail(M, ...) fprintf(stderr, "[DEBUG] (%s:%d:%s) " M "\n",\
                  __FILE__, __LINE__, __func__, ##__VA_ARGS__)
/* 简要调试 */
#define debug(M, ...) fprintf(stderr, "[DEBUG] " M "\n", ##__VA_ARGS__)
/* log: DEBUG 模式下同时输出到 stderr 和日志文件 */
#define log(M, ...) {log_info (M, ##__VA_ARGS__); log_file (M, ##__VA_ARGS__);}
#else
/* 非 DEBUG 模式: debug 宏为空，log 只输出到文件 */
#define debug(M, ...)
#define log(M, ...) {log_file (M, ##__VA_ARGS__);}
#endif

#endif /* DEBUG_H_ */
