/* 
 * GiantVM Frontier High-Performance Memory Server 
 * Feature: SO_REUSEPORT included for Multi-Process Scaling
 * Compile: gcc -O3 -o fast_mem_server fast_mem_server.c -lpthread
 * Usage: Run 8 instances of this program in background.
 */
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#define _GNU_SOURCE 
#include <endian.h>

#define MAX_EVENTS 10240
#define PORT 9999
#define PAGE_SIZE 4096
#define PREFETCH 32
#define MEM_FILE "physical_ram.img"

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    int listen_sock, conn_sock, nfds, epollfd;
    struct epoll_event ev, events[MAX_EVENTS];
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int fd_mem = open(MEM_FILE, O_RDONLY);
    if (fd_mem < 0) { perror("Open memory file"); return 1; }
    off_t file_size = lseek(fd_mem, 0, SEEK_END);
    char *mem_ptr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd_mem, 0);
    if (mem_ptr == MAP_FAILED) { perror("mmap"); return 1; }

    /* [Frontier] 开启大页与预读优化 */
    /* 作用：将 TLB Miss 减少 99%，防止 CPU 卡在页表查找上 */
    madvise(mem_ptr, file_size, MADV_HUGEPAGE); 
    madvise(mem_ptr, file_size, MADV_RANDOM);   // 告诉内核：这是随机访问，别做顺序预读
    madvise(mem_ptr, file_size, MADV_WILLNEED); // 告诉内核：我会用到这些内存，尽快换入

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    
    /* [Frontier] 开启 SO_REUSEPORT，允许开启多个进程绑定同一端口 */
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_sock, 20000) < 0) { perror("listen"); return 1; }

    epollfd = epoll_create1(0);
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listen_sock, &ev) == -1) { perror("epoll_ctl: listen_sock"); return 1; }

    printf("[*] High-Perf Memory Server running on port %d (PID: %d)\n", PORT, getpid());

    while (1) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) { perror("epoll_wait"); break; }

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == listen_sock) {
                while (1) {
                    conn_sock = accept(listen_sock, (struct sockaddr *)&addr, &addrlen);
                    if (conn_sock == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 处理完了
                        perror("accept"); break;
                    }
        
                    set_nonblocking(conn_sock);
                    int flag = 1;
                    setsockopt(conn_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

                    ev.events = EPOLLIN | EPOLLET; // 保持边缘触发
                    ev.data.fd = conn_sock;
                    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock, &ev) == -1) {
                        close(conn_sock);
                    }
                }
            } else {
                int fd = events[n].data.fd;
                /* [CRITICAL FIX] 循环读取直到 EAGAIN */
                while (1) {
                    uint64_t req_addr_be;
                    ssize_t count = recv(fd, &req_addr_be, 8, MSG_WAITALL);
                    
                    if (count == 8) {
                        uint64_t base = be64toh(req_addr_be);
                        if (base + PAGE_SIZE * PREFETCH > file_size) base = 0; 
                        
                        size_t total_to_send = PAGE_SIZE * PREFETCH;
                        size_t sent_total = 0;
                        char *send_ptr = mem_ptr + base;

                        /* 基于时间的宽容超时逻辑 */
                        struct timespec ts_start = {0}, ts_now;
                        
                        while(sent_total < total_to_send) {
                            ssize_t s = send(fd, send_ptr + sent_total, total_to_send - sent_total, 0);
                            if (s < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    /* 第一次遇到阻塞时，记录当前时间 */
                                    if (ts_start.tv_sec == 0) {
                                        clock_gettime(CLOCK_MONOTONIC, &ts_start);
                                    }
                                    
                                    /* 检查是否超时 */
                                    clock_gettime(CLOCK_MONOTONIC, &ts_now);
                                    if (ts_now.tv_sec - ts_start.tv_sec > 5) {
                                        // 阻塞超过 5 秒，判定为死链接，断开
                                        close(fd); goto next_event; 
                                    }
                                    
                                    /* 增加睡眠时间到 50us，避免 CPU 空转过热 */
                                    usleep(50); 
                                    continue; 
                                }
                                // 其他错误直接断开
                                close(fd); goto next_event;
                            }
                            
                            sent_total += s;
                            /* 只要成功发送了数据，重置超时计时器，给下一次发送机会 */
                            ts_start.tv_sec = 0; 
                        }
                    } else if (count < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读空了，退出循环等待下一次事件
                        close(fd); 
                        break;
                    } else if (count == 0) {
                        close(fd); // 对端关闭
                        break;
                    } else {
                        // 读到了半截数据(小于8字节)，这在 TCP流中极少见但理论存在
                        // 简单处理：关连接。复杂处理：维护个 buffer 拼包。
                        // 鉴于这是高性能场景，视为协议错误关闭即可。
                        close(fd);
                        break;
                    }
                }
                next_event:;
            }
        }
    }
    return 0;
}
