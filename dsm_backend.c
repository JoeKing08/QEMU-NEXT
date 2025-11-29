#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "dsm_backend.h"
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sched.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>

#define _GNU_SOURCE 
#include <endian.h>

#ifndef __NR_userfaultfd
#define __NR_userfaultfd 323
#endif

/* === 配置区域 === */
#define PREFETCH 32           // 预取页面数量 (32 * 4KB = 128KB)
#define PAGE_SIZE 4096
#define MAX_OPEN_SOCKETS 10240 // 允许同时保持的最大连接数
#define WORKER_THREADS 64     // UFFD 处理线程数

// 节点 IP 列表 (请按实际情况填写)
const char *NODE_IPS[] = { 
    "192.168.1.101", 
    "192.168.1.102", 
    // ... 更多节点
}; 
#define NODE_COUNT (sizeof(NODE_IPS)/sizeof(NODE_IPS[0]))

/* === 全局变量 === */
int gvm_mode = 0;              // 0: Kernel Mode, 1: User Mode
int uffd_fd = -1;
int *global_sockets = NULL;    // 存储 socket fd
pthread_mutex_t *conn_locks = NULL; // 每个节点的专用锁
int active_node_ids[MAX_OPEN_SOCKETS]; 
int next_victim_idx = 0;
pthread_mutex_t lru_lock = PTHREAD_MUTEX_INITIALIZER; 

/* 
 * 建立连接底层函数 
 * 包含：超时设置、KeepAlive、NoDelay
 */
static int connect_node_impl(const char *ip) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    /* 1. 暴力复位 (Linger 0)，防止 TIME_WAIT 耗尽端口 */
    struct linger sl = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(s, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));

    /* 2. 基础性能优化 */
    int flag = 1, bufsize = 4 * 1024 * 1024;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&bufsize, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&bufsize, sizeof(int));

    /* 3. KeepAlive (防止静默断开) */
    int keepalive = 1, keepidle = 5, keepintvl = 2, keepcnt = 3;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(int));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(int));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(int));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(int));

    /* 4. [FIX] 读写超时 (防止死锁的关键) */
    struct timeval timeout;
    timeout.tv_sec = 2;  // 2秒无响应即视为断开，触发重连
    timeout.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    struct sockaddr_in a;
    a.sin_family = AF_INET;
    a.sin_port = htons(9999);
    inet_pton(AF_INET, ip, &a.sin_addr);

    if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) { 
        close(s); 
        return -1; 
    }
    return s;
}

/* 
 * [CRITICAL FIX] 获取连接并锁定
 * 返回值: Socket FD (>=0) 表示成功，且 conn_locks[node_id] 处于 LOCKED 状态。
 *        -1 表示失败，锁已被释放。
 */
static int get_or_connect_locked(int node_id) {
    if (node_id < 0 || node_id >= NODE_COUNT) return -1;

    pthread_mutex_lock(&conn_locks[node_id]);

    /* 极其简单：有就用，没有就连，不需要踢人 */
    if (global_sockets[node_id] != -1) {
        return global_sockets[node_id];
    }

    /* 建立连接 */
    int s = connect_node_impl(NODE_IPS[node_id]);
    if (s >= 0) {
        global_sockets[node_id] = s;
        return s;
    } else {
        pthread_mutex_unlock(&conn_locks[node_id]);
        return -1;
    }
}

/* 
 * 辅助函数：关闭连接并清理状态 
 * 调用前必须持有 conn_locks[node_id]
 */
static void close_socket_locked(int node_id) {
    int s = global_sockets[node_id];
    if (s != -1) {
        close(s);
        global_sockets[node_id] = -1;
    }
}

/* 工作线程 - 修复版 (包含重试与防死锁兜底) */
void *dsm_worker(void *arg) {
    const int BATCH_SIZE = 64;
    struct uffd_msg msgs[BATCH_SIZE];
    ssize_t nread;
    
    // 分配接收缓冲区
    char *buf = malloc(PAGE_SIZE * PREFETCH);
    if (!buf) return NULL;

    // 提高线程优先级，确保缺页响应速度
    struct sched_param p = { .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_RR, &p);

    // 随机种子用于 Jitter
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)pthread_self();

    while(1) {
        /* 读取事件 */
        struct pollfd pfd = { .fd = uffd_fd, .events = POLLIN };
        int poll_ret = poll(&pfd, 1, 2000); // 2秒超时
        
        if (poll_ret <= 0) continue;

        nread = read(uffd_fd, msgs, sizeof(struct uffd_msg) * BATCH_SIZE);
        if (nread <= 0) continue;

        int n_events = nread / sizeof(struct uffd_msg);

        for (int i = 0; i < n_events; i++) {
            struct uffd_msg *msg = &msgs[i];
            if (!(msg->event & UFFD_EVENT_PAGEFAULT)) continue;

            uint64_t addr = msg->arg.pagefault.address;
            uint64_t base = addr & ~(4095);
            int owner = (base / 4096) % NODE_COUNT;

            /* 1. 注入 Jitter (可选，缓解服务端并发压力) */
            // if ((rand_r(&seed) % 100) < 5) usleep(rand_r(&seed) % 10);

            /* 2. [CRITICAL FIX] 获取连接 (带重试机制) */
            int sock = -1;
            int retry_cnt = 0;
            const int MAX_RETRIES = 5;

            while (retry_cnt < MAX_RETRIES) {
                sock = get_or_connect_locked(owner);
                if (sock >= 0) break; // 成功拿到锁定的连接

                retry_cnt++;
                // 简单的线性退避: 1ms, 2ms, 3ms...
                usleep(1000 * retry_cnt); 
            }
            
            /* 3. [CRITICAL FIX] 兜底逻辑：防止死锁 */
            if (sock < 0) {
                fprintf(stderr, "[DSM] FATAL: Node %d unreachable after retries. Zero-filling %lx to unblock vCPU.\n", owner, base);
                
                // 必须填充一个零页，否则 vCPU 会永远停在这一行汇编指令上等待
                struct uffdio_zeropage z = { 
                    .range = { .start = base, .len = 4096 }, 
                    .mode = 0 
                };
                if (ioctl(uffd_fd, UFFDIO_ZEROPAGE, &z) < 0) {
                    // 如果页面已存在(EEXIST)，唤醒即可
                    if (errno == EEXIST) {
                         struct uffdio_wake w = { .start = base, .len = 4096, .mode = 0 };
                         ioctl(uffd_fd, UFFDIO_WAKE, &w);
                    } else {
                        perror("[DSM] Emergency zeropage failed");
                    }
                }
                
                // 跳过后续网络操作，处理下一个事件
                continue; 
            }

            /* 4. 发送请求 */
            uint64_t req = htobe64(base);
            if (send(sock, &req, 8, 0) != 8) {
                close_socket_locked(owner); // 发送失败，标记连接失效
                pthread_mutex_unlock(&conn_locks[owner]); // 必须解锁
                
                // 这里其实也可以 goto 到上面的 Zero-fill 逻辑，
                // 但简单起见，让 VM 再触发一次缺页重试通常更安全
                continue;
            }

            /* 5. 接收数据 */
            int total = PAGE_SIZE * PREFETCH;
            int recvd = 0;
            bool error = false;

            while (recvd < total) {
                int n = recv(sock, buf + recvd, total - recvd, MSG_WAITALL);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                         // 超时视为连接坏死
                    }
                    close_socket_locked(owner); 
                    error = true;
                    break;
                }
                recvd += n;
            }

            /* 6. [CRITICAL] 任务完成，解锁 */
            pthread_mutex_unlock(&conn_locks[owner]);

            if (error) {
                // 接收失败，不进行 UFFD 操作，让 VM 指令重试，
                // 下次重试时会重新触发 PageFault，进入重连逻辑
                continue; 
            }

            /* 7. 填充内存 (无需锁) */
            // 注意：虽然请求了 PREFETCH 大小，但 UFFD 只需要填充触发缺页的那个 base
            // 不过为了性能，我们可以把预取的数据都填进去（如果内核支持）
            for(int k=0; k<PREFETCH; k++) {
                struct uffdio_copy c = {
                    .dst = base + k*4096, 
                    .src = (uint64_t)(buf + k*4096), 
                    .len = 4096, 
                    .mode = 0 
                };
                
                // 重点：如果目标地址超出了注册范围，或者已被映射，ioctl 会失败
                // 我们主要关心 k=0 (当前缺页地址) 的成功
                if (ioctl(uffd_fd, UFFDIO_COPY, &c) < 0) {
                    if (errno == EEXIST) {
                        // 竞争条件下，页面可能已经被别的线程填了，唤醒即可
                        struct uffdio_wake w = { .start = c.dst, .len = c.len, .mode = 0 };
                        ioctl(uffd_fd, UFFDIO_WAKE, &w);
                    }
                }
            }
        }
    }
    free(buf);
    return NULL;
}

/* 初始化入口 */
void dsm_universal_init(void) {
    signal(SIGPIPE, SIG_IGN); // 忽略 Broken Pipe

    /* 检测内核模块是否存在 */
    if (access("/sys/module/giantvm_kvm", F_OK) == 0 || access("/dev/giantvm", F_OK) == 0) {
        printf("[GiantVM] KERNEL MODULE DETECTED. Using ORIGINAL FULL-SHARE Mode.\n");
        gvm_mode = 0; 
        return; 
    }

    printf("[GiantVM] NO KERNEL MODULE. Using MEMORY-ONLY Mode (Frontier Fixed).\n");
    gvm_mode = 1; 
    
    uffd_fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd_fd < 0) {
        perror("[GiantVM] userfaultfd syscall failed");
        return;
    }

    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(uffd_fd, UFFDIO_API, &api) < 0) {
        perror("[GiantVM] uffdio_api failed");
        close(uffd_fd);
        uffd_fd = -1;
        return;
    }
    
    /* 分配全局结构 */
    global_sockets = malloc(sizeof(int) * NODE_COUNT);
    conn_locks = malloc(sizeof(pthread_mutex_t) * NODE_COUNT);
    
    if (!global_sockets || !conn_locks) abort(); // OOM 必须死

    for(int i=0; i<NODE_COUNT; i++) {
        global_sockets[i] = -1;
        pthread_mutex_init(&conn_locks[i], NULL);
    }
    for(int i=0; i<MAX_OPEN_SOCKETS; i++) {
        active_node_ids[i] = -1;
    }

    /* 启动 Worker 线程 */
    for(int i=0; i < WORKER_THREADS; i++) {
        qemu_thread_create(NULL, "dsm-w", dsm_worker, NULL, QEMU_THREAD_JOINABLE);
    }
}

/* 注册内存区域 */
void dsm_universal_register(void *ptr, size_t size) {
    if (gvm_mode == 1 && uffd_fd >= 0) {
        struct uffdio_register r = { 
            .range = {(uint64_t)ptr, size}, 
            .mode = UFFDIO_REGISTER_MODE_MISSING 
        };
        if (ioctl(uffd_fd, UFFDIO_REGISTER, &r) < 0) {
             perror("[GiantVM] register memory failed");
        }
    }
}
