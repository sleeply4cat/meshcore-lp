// Minimal nrfx glue for a bare-metal, single-threaded build (no RTOS, no SoftDevice).
// Only the HAL headers of nrfx are used (nrf_clock.h via TinyUSB), no nrfx drivers.
#ifndef NRFX_GLUE_H__
#define NRFX_GLUE_H__


#include <soc/nrfx_coredep.h>
#include <soc/nrfx_atomic.h>

#define NRFX_ASSERT(expression)          do { } while (0)
#define NRFX_STATIC_ASSERT(expression)   _Static_assert(expression, "")

#define NRFX_IRQ_PRIORITY_SET(irq_number, priority) NVIC_SetPriority(irq_number, priority)
#define NRFX_IRQ_ENABLE(irq_number)       NVIC_EnableIRQ(irq_number)
#define NRFX_IRQ_IS_ENABLED(irq_number)   (0 != (NVIC->ISER[irq_number / 32u] & (1u << (irq_number % 32u))))
#define NRFX_IRQ_DISABLE(irq_number)      NVIC_DisableIRQ(irq_number)
#define NRFX_IRQ_PENDING_SET(irq_number)  NVIC_SetPendingIRQ(irq_number)
#define NRFX_IRQ_PENDING_CLEAR(irq_number) NVIC_ClearPendingIRQ(irq_number)
#define NRFX_IRQ_IS_PENDING(irq_number)   (NVIC_GetPendingIRQ(irq_number) == 1)

#define NRFX_CRITICAL_SECTION_ENTER()   { uint32_t __pm = __get_PRIMASK(); __disable_irq();
#define NRFX_CRITICAL_SECTION_EXIT()      if (!__pm) __enable_irq(); }

#define NRFX_DELAY_DWT_BASED    0
#define NRFX_DELAY_US(us_time)  nrfx_coredep_delay_us(us_time)

#define nrfx_atomic_t           nrfx_atomic_u32_t
#define NRFX_ATOMIC_FETCH_STORE(p_data, value)  nrfx_atomic_u32_fetch_store(p_data, value)
#define NRFX_ATOMIC_FETCH_OR(p_data, value)     nrfx_atomic_u32_fetch_or(p_data, value)
#define NRFX_ATOMIC_FETCH_AND(p_data, value)    nrfx_atomic_u32_fetch_and(p_data, value)
#define NRFX_ATOMIC_FETCH_XOR(p_data, value)    nrfx_atomic_u32_fetch_xor(p_data, value)
#define NRFX_ATOMIC_FETCH_ADD(p_data, value)    nrfx_atomic_u32_fetch_add(p_data, value)
#define NRFX_ATOMIC_FETCH_SUB(p_data, value)    nrfx_atomic_u32_fetch_sub(p_data, value)
#define NRFX_ATOMIC_CAS(p_data, old_value, new_value) nrfx_atomic_u32_cmp_exch(p_data, &old_value, new_value)

#define NRFX_CLZ(value) __CLZ(value)
#define NRFX_CTZ(value) __CLZ(__RBIT(value))

#define NRFX_CUSTOM_ERROR_CODES 0
#define NRFX_EVENT_READBACK_ENABLED 1

#define NRFX_DPPI_CHANNELS_USED   0
#define NRFX_DPPI_GROUPS_USED     0
#define NRFX_PPI_CHANNELS_USED    0
#define NRFX_PPI_GROUPS_USED      0
#define NRFX_GPIOTE_CHANNELS_USED 0
#define NRFX_EGUS_USED            0
#define NRFX_TIMERS_USED          0

#endif
