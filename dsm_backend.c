/* dsm_backend.c */
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

#ifndef __NR_userfaultfd
#define __NR_userfaultfd 323
#endif

// === 配置区 ===
#define THREAD_COUNT 8 
#define PREFETCH_COUNT 32
#define TCP_BUF_SIZE (2 * 1024 * 1024)
#define PAGE_SIZE 4096

// [部署必改] 集群内存节点 IP 列表
const char *NODE_IPS[] = {
    "127.0.0.1", 
    // "192.168.1.101",
    // "192.168.1.102"
};
#define NODE_COUNT (sizeof(NODE_IPS)/sizeof(NODE_IPS[0]))

#define PROTO_ZERO 0xAA
#define PROTO_DATA 0xBB

int dsm_mode = 0; // 0: Off, 1: KVM+UFFD, 2: TCG+UFFD
int uffd_fd = -1;
int global_sockets[128];

static void optimize_sock(int s) {
    int f = 1, b = TCP_BUF_SIZE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&f, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&b, sizeof(int));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&b, sizeof(int));
}

static int connect_node(const char *ip) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    optimize_sock(s);
    struct sockaddr_in a;
    a.sin_family = AF_INET;
    a.sin_port = htons(9999);
    if (inet_pton(AF_INET, ip, &a.sin_addr)<=0) { close(s); return -1; }
    if (connect(s, (struct sockaddr*)&a, sizeof(a))<0) { close(s); return -1; }
    return s;
}

void *dsm_worker(void *arg) {
    struct uffd_msg msg;
    char buf[PAGE_SIZE * PREFETCH_COUNT];
    
    // 尝试提权
    struct sched_param p = { .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_RR, &p);

    int my_socks[128];
    for(int i=0; i<NODE_COUNT; i++) my_socks[i] = connect_node(NODE_IPS[i]);

    while(1) {
        if (read(uffd_fd, &msg, sizeof(msg)) != sizeof(msg)) continue;
        if (msg.event & UFFD_EVENT_PAGEFAULT) {
            uint64_t addr = msg.arg.pagefault.address;
            uint64_t base = addr & ~(4095);
            
            int owner = (base / 4096) % NODE_COUNT;
            int sock = my_socks[owner];
            if (sock < 0) continue;

            uint64_t req = htobe64(base);
            if (send(sock, &req, 8, 0) != 8) continue;

            for (int i=0; i<PREFETCH_COUNT; i++) {
                uint64_t curr = base + (i*4096);
                uint8_t type;
                if (recv(sock, &type, 1, 0) <= 0) break;
                
                if (type == PROTO_ZERO) {
                    struct uffdio_zeropage z = { .range = {curr, 4096}, .mode = 0 };
                    ioctl(uffd_fd, UFFDIO_ZEROPAGE, &z);
                } else {
                    int recvd = 0;
                    while (recvd < 4096) {
                        int n = recv(sock, buf + recvd, 4096 - recvd, 0);
                        if (n<=0) goto next;
                        recvd += n;
                    }
                    struct uffdio_copy c = { .dst = curr, .src = (uint64_t)buf, .len = 4096, .mode = 0 };
                    ioctl(uffd_fd, UFFDIO_COPY, &c);
                }
            }
            next:;
        }
    }
    return NULL;
}

void dsm_param_init(void) { } // 预留接口

void dsm_auto_setup(void) {
    bool has_kvm = (access("/dev/kvm", R_OK|W_OK) == 0);
    uffd_fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd_fd >= 0) {
        struct uffdio_api api = { .api = UFFD_API, .features = 0 };
        if (ioctl(uffd_fd, UFFDIO_API, &api) == 0) {
            dsm_mode = has_kvm ? 1 : 2;
            printf("[GiantVM] ULT Mode: ON (KVM: %d, Nodes: %ld)\n", has_kvm, NODE_COUNT);
            for(int i=0; i<THREAD_COUNT; i++) {
                char n[32]; snprintf(n, 32, "dsm-%d", i);
                qemu_thread_create(NULL, n, dsm_worker, NULL, QEMU_THREAD_JOINABLE);
            }
        }
    }
}

void dsm_register_ram(void *ptr, size_t size) {
    if (dsm_mode) {
        struct uffdio_register r = { .range = {(uint64_t)ptr, size}, .mode = UFFDIO_REGISTER_MODE_MISSING };
        ioctl(uffd_fd, UFFDIO_REGISTER, &r);
    }
}
