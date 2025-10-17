#include "cmsis_os.h"
#include <stdint.h>
#include <string.h>

/* COE718 Lab 3a Q2 - Analysis version */

// ------------------- Watch-friendly globals -------------------
volatile uint32_t mem_access_counter = 0;
volatile uint32_t cpu_access_counter = 0;
volatile uint32_t app_counter        = 0;
volatile uint32_t dev_counter        = 0;
volatile uint32_t ui_user_count      = 0;

/* Make these volatile so the Watch window always sees updates */
volatile uint32_t bb_word = 0;
volatile char     logger[128] = {0};

volatile const char *logger_str   = logger;
// ------------------- Timeline monitor -------------
static char timeline[128];
static volatile uint32_t tl_head = 0;
osMutexDef(tl_mutex);
static osMutexId tl_mutex;

volatile const char *timeline_str = timeline;

static __inline void tl_mark(char tag) {
  osMutexWait(tl_mutex, osWaitForever);
  if (tl_head < sizeof(timeline) - 1) {
    if (tl_head == 0 || timeline[tl_head - 1] != tag) {
      timeline[tl_head++] = tag;
      timeline[tl_head]   = '\0';
    }
  }
  osMutexRelease(tl_mutex);
}

// ------------------- Signals -------------------
#define SIG_MM_TO_CPU   (1U << 0)
#define SIG_CPU_TO_MM   (1U << 1)
#define SIG_APP_READY   (1U << 2)
#define SIG_DEV_DONE    (1U << 3)

// ------------------- Single global mutex for logger ------------
osMutexDef(log_mutex);
static osMutexId log_mutex;

// ------------------- Bit-band helpers (SRAM) -------------------
#define BB_SRAM_REF   (0x20000000UL)
#define BB_SRAM_ALIAS (0x22000000UL)

static __inline volatile uint32_t* bb_alias_addr(volatile void* addr, uint32_t bit)
{
  uint32_t byte_offset = (uint32_t)addr - BB_SRAM_REF;
  uint32_t bit_word_offset = (byte_offset * 32U) + (bit * 4U);
  return (volatile uint32_t*)(BB_SRAM_ALIAS + bit_word_offset);
}
static __inline void bb_write_bit(volatile void* addr, uint32_t bit, uint32_t val)
{
  *bb_alias_addr(addr, bit) = (val ? 1U : 0U);
}
static __inline uint32_t bb_read_bit(volatile void* addr, uint32_t bit)
{
  return *bb_alias_addr(addr, bit);
}

// ------------------- Rotate-right (barrel-shift demo) ----------
static __inline uint32_t ror32(uint32_t x, unsigned n)
{
  n &= 31U;
  return (x >> n) | (x << (32U - n));
}

// ------------------- Thread IDs & defs -------------------------
static osThreadId tid_mem, tid_cpu, tid_app, tid_dev, tid_ui, tid_mon;

void Th_MemoryManagement(const void *arg);
void Th_CPUManagement(const void *arg);
void Th_ApplicationInterface(const void *arg);
void Th_DeviceManagement(const void *arg);
void Th_UserInterface(const void *arg);
void Monitor(const void *arg);

/* Explicit stacks */
osThreadDef(Th_MemoryManagement,    osPriorityNormal, 1, 0);
osThreadDef(Th_CPUManagement,       osPriorityNormal, 1, 0);
osThreadDef(Th_ApplicationInterface,osPriorityNormal, 1, 0);
osThreadDef(Th_DeviceManagement,    osPriorityNormal, 1, 0);
osThreadDef(Th_UserInterface,       osPriorityNormal, 1, 0);
osThreadDef(Monitor,                osPriorityLow,    1, 0);

int Init_Thread (void) {
  tl_mutex  = osMutexCreate(osMutex(tl_mutex));
  log_mutex = osMutexCreate(osMutex(log_mutex));

  tid_mem = osThreadCreate(osThread(Th_MemoryManagement),    NULL);
  tid_cpu = osThreadCreate(osThread(Th_CPUManagement),       NULL);
  tid_app = osThreadCreate(osThread(Th_ApplicationInterface),NULL);
  tid_dev = osThreadCreate(osThread(Th_DeviceManagement),    NULL);
  tid_ui  = osThreadCreate(osThread(Th_UserInterface),       NULL);
  if (!tid_mem || !tid_cpu || !tid_app || !tid_dev || !tid_ui) return -1;

  /* Finite monitor */
  tid_mon = osThreadCreate(osThread(Monitor), NULL);
  return 0;
}

// ------------------- Implementations --------------------------
void Th_MemoryManagement(const void *arg)
{
  uint32_t b0;
  mem_access_counter++;
  tl_mark('M');

  /* Bit-band demo: set bit3, clear bit2, toggle bit0 */
  bb_write_bit((void*)&bb_word, 3U, 1U);
  bb_write_bit((void*)&bb_word, 2U, 0U);
  b0 = bb_read_bit((void*)&bb_word, 0U);
  bb_write_bit((void*)&bb_word, 0U, b0 ^ 1U);

  /* Signal CPU and wait for response */
  osSignalSet(tid_cpu, SIG_MM_TO_CPU);
  (void)osSignalWait(SIG_CPU_TO_MM, osWaitForever);

  osDelay(1);
  osThreadTerminate(osThreadGetId());
}

void Th_CPUManagement(const void *arg)
{
  uint32_t x;
  unsigned rot;

  (void)osSignalWait(SIG_MM_TO_CPU, osWaitForever);
  tl_mark('C');

  cpu_access_counter++;

  /* Conditional rotate: if bit3 set in bb_word, rotate by 7 else by 3 */
  x = (uint32_t)bb_word;
  rot = bb_read_bit((void*)&bb_word, 3U) ? 7U : 3U;
  x = ror32(x ^ 0xA5A5A5A5UL, rot);

  osSignalSet(tid_mem, SIG_CPU_TO_MM);
  osThreadTerminate(osThreadGetId());
}

void Th_ApplicationInterface(const void *arg)
{
  tl_mark('A');

  osMutexWait(log_mutex, osWaitForever);
  strcpy((char*)logger, "App: begin write -> ");
  osMutexRelease(log_mutex);

  osSignalSet(tid_dev, SIG_APP_READY);
  (void)osSignalWait(SIG_DEV_DONE, osWaitForever);

  app_counter++;
  osDelay(1);
  osThreadTerminate(osThreadGetId());
}

void Th_DeviceManagement(const void *arg)
{
  (void)osSignalWait(SIG_APP_READY, osWaitForever);
  tl_mark('D');

  osMutexWait(log_mutex, osWaitForever);
  strcat((char*)logger, "Device: append + close.");
  osMutexRelease(log_mutex);

  osSignalSet(tid_app, SIG_DEV_DONE);

  dev_counter++;
  osDelay(1);
  osThreadTerminate(osThreadGetId());
}

void Th_UserInterface(const void *arg)
{
  tl_mark('U');
  ui_user_count++;
  //osDelay(1);
  osThreadTerminate(osThreadGetId());
}

/* Finite Monitor: terminates after ~2s */
void Monitor(const void *arg)
{
  static volatile uint32_t hb = 0;
  uint32_t i;
  for (i = 0; i < 40U; ++i) {  /* 40 * 50 ms = ~2 seconds */
    hb++;
    osDelay(50);
  }
  osThreadTerminate(osThreadGetId());
}

/* Dummy template symbols (not used) */
void Thread1 (void const *argument) { for(;;) { osDelay(1000); } }
void Thread2 (void const *argument) { for(;;) { osDelay(1000); } }
