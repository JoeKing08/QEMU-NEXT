/* dsm_backend.h - GiantVM Ultimate Extension */
#ifndef DSM_BACKEND_H
#define DSM_BACKEND_H
#include <stddef.h>
#include <stdint.h>

extern int dsm_mode;
void dsm_param_init(void);
void dsm_auto_setup(void);
void dsm_register_ram(void *ptr, size_t size);
#endif
