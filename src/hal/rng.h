#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     rng_fill(void* out, size_t len);
uint32_t rng_u32(void);

#ifdef __cplusplus
}
#endif
