#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVMC_PAGE_SIZE 4096u

void nvmc_erase_page(uint32_t addr);
void nvmc_write_words(uint32_t addr, const uint32_t* data, uint32_t nwords);

#ifdef __cplusplus
}
#endif
