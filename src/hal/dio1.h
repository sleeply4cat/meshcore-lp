#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void     dio1_init(void);
bool     dio1_level(void);
uint32_t dio1_edges(void);
uint64_t dio1_last_edge_ticks(void);

#ifdef __cplusplus
}
#endif
