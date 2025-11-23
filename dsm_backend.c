#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "dsm_backend.h"
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
#include <stdio.h>

#ifndef __NR_userfaultfd
#define __NR_userfaultfd 323
#endif

// === 配置区 ===
#define THREAD_COUNT 8
#define PREFETCH 32
#define PAGE_SIZE 4096
#define TCP_BUF_SIZE (2 * 1024 * 1024)

// 全局状态
int gvm_mode = 0; 
int uffd_fd = -1;

// UFFD模式下的节点管理
char **node_ips = NULL;
int node_count = 0;

static void optimize_socket(int s) {
    int f=1, b=TCP_BUF_SIZE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&f, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&b, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&b, sizeof(int));
}

static int connect_node(const char *ip) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    optimize_socket(s);
    struct sockaddr_in a;
    a.sin_family = AF_INET;
    a.sin_port = htons(9999); // 内存服务默认端口
    if (inet_pton(AF_INET, ip, &a.sin_addr)<=0) { close(s); return -1; }
    if (connect(s, (struct sockaddr*)&a, sizeof(a))<0) { close(s); return -1; }
    return s;
}

// 读取配置文件 cluster_uffd.conf
static void load_config(void) {
    FILE *f = fopen("cluster_uffd.conf", "r");
    if (!f) {
        // 默认单机回环
        node_count = 1;
        node_ips = malloc(sizeof(char*));
        node_ips[0] = strdup("127.0.0.1");
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) if(line[0]!='\n') node_count++;
    rewind(f);
    node_ips = malloc(sizeof(char*) * node_count);
    int i=0;
    while (fgets(line, sizeof(line), f) && i<node_count) {
        line[strcspn(line, "\n")] = 0;
        node_ips[i++] = strdup(line);
    }
    fclose(f);
}

void *dsm_worker(void *arg) {
    struct uffd_msg msg;
    char buf[PAGE_SIZE * PREFETCH];
    
    // 尝试开启实时优先级 (需sudo)
    struct sched_param p = { .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_RR, &p);

    int *socks = malloc(sizeof(int)*node_count);
    for(int i=0; i<node_count; i++) socks[i] = connect_node(node_ips[i]);

    while(1) {
        if (read(uffd_fd, &msg, sizeof(msg)) != sizeof(msg)) continue;
        if (msg.event & UFFD_EVENT_PAGEFAULT) {
            uint64_t addr = msg.arg.pagefault.address;
            uint64_t base = addr & ~(4095);
            
            // 简单的取模分片
            int owner = (base / 4096) % node_count;
            int s = socks[owner];
            if (s < 0) continue;

            uint64_t req = htobe64(base);
            if (send(s, &req, 8, 0) != 8) continue;

            int total = PAGE_SIZE * PREFETCH;
            int recvd = 0;
            while(recvd < total) {
                int n = recv(s, buf + recvd, total - recvd, 0);
                if (n<=0) break;
                recvd += n;
            }

            for(int k=0; k<PREFETCH; k++) {
                struct uffdio_copy c = {
                    .dst = base + k*4096,
                    .src = (uint64_t)(buf + k*4096),
                    .len = 4096, .mode = 0
                };
                ioctl(uffd_fd, UFFDIO_COPY, &c);
            }
        }
    }
    return NULL;
}

// === 核心分流逻辑 ===
void dsm_universal_init(void) {
    // 1. 检测 GiantVM 内核模块是否加载
    // GiantVM 模块加载后通常会创建 /dev/giantvm
    if (access("/dev/giantvm", F_OK) == 0) {
        printf("[GiantVM] DETECTED CUSTOM KERNEL. Switching to ORIGINAL Mode.\n");
        gvm_mode = 0; // 关闭 UFFD 逻辑，交还给原版代码
        return;
    }

    // 2. 无内核模块，进入 UFFD 模式
    printf("[GiantVM] NO Custom Kernel. Switching to MEMORY-ONLY (UFFD) Mode.\n");
    gvm_mode = 1;

    // 检测标准 KVM 状态 (仅做提示)
    if (access("/dev/kvm", R_OK|W_OK) == 0) {
        printf("[GiantVM] Standard KVM detected. Using Hardware Acceleration.\n");
    } else {
        printf("[GiantVM] No KVM. Falling back to TCG (Software).\n");
    }

    load_config();
    uffd_fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd_fd >= 0) {
        struct uffdio_api api = { .api = UFFD_API, .features = 0 };
        ioctl(uffd_fd, UFFDIO_API, &api);
        // 启动 worker 线程池
        for(int i=0; i<THREAD_COUNT; i++) {
            qemu_thread_create(NULL, "dsm-w", dsm_worker, NULL, QEMU_THREAD_JOINABLE);
        }
    } else {
        printf("[GiantVM] FATAL: Failed to init Userfaultfd.\n");
    }
}

void dsm_universal_register(void *ptr, size_t size) {
    // 只有在 UFFD 模式下才手动注册
    // 原版模式下，GiantVM 的 ioctl 会自己处理
    if (gvm_mode == 1 && uffd_fd >= 0) {
        struct uffdio_register r = { .range = {(uint64_t)ptr, size}, .mode = UFFDIO_REGISTER_MODE_MISSING };
        ioctl(uffd_fd, UFFDIO_REGISTER, &r);
    }
}
