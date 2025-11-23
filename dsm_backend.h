/* GiantVM Ultimate Extension */
#ifndef DSM_BACKEND_H
#define DSM_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* 全局模式标志：0=原版内核模式, 1=UFFD内存模式 */
extern int gvm_mode;

void dsm_universal_init(void);
void dsm_universal_register(void *ptr, size_t size);

#endif
