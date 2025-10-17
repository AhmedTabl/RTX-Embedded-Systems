#include "cmsis_os.h"
#include "LPC17xx.h"
#include "GLCD.h"
#include <stdint.h>
#include <string.h>

/* ===== ~3-second window per task (RTX tick = 5 ms) ===== */
#ifndef RTX_TICK_US
# define RTX_TICK_US         5000u
#endif
#ifndef WINDOW_TICKS
# define WINDOW_TICKS        600u   /* 600 * 5 ms ˜ 3.0 s */
#endif

/* ------------------- Shared demo/analysis state ------------------- */
volatile uint32_t mem_access_counter = 0;
volatile uint32_t cpu_access_counter = 0;
volatile uint32_t app_counter        = 0;
volatile uint32_t dev_counter        = 0;
volatile uint32_t ui_user_count      = 0;

/* Bit-band demo target and shared logger */
volatile uint32_t bb_word = 0;
volatile char     logger[128] = {0};

/* ------------------- Signals ------------------- */
#define SIG_MM_TO_CPU   (1u << 0)
#define SIG_CPU_TO_MM   (1u << 1)
#define SIG_APP_READY   (1u << 2)
#define SIG_DEV_DONE    (1u << 3)
#define SIG_UI_DONE     (1u << 4)  // Added signal for UI done

/* ------------------- Mutexes ------------------- */
osMutexDef(log_mutex);
static osMutexId log_mutex;

osMutexDef(lcd_mutex);
static osMutexId lcd_mutex;

/* ------------------- Bit-band helpers (SRAM) ------------------- */
#define BB_SRAM_REF   (0x20000000UL)
#define BB_SRAM_ALIAS (0x22000000UL)

static volatile uint32_t* bb_alias_addr(volatile void* addr, uint32_t bit)
{
  uint32_t byte_off = (uint32_t)addr - BB_SRAM_REF;
  uint32_t word_off = (byte_off * 32u) + (bit * 4u);
  return (volatile uint32_t*)(BB_SRAM_ALIAS + word_off);
}

static void bb_write_bit(volatile void* addr, uint32_t bit, uint32_t val)
{
  *bb_alias_addr(addr, bit) = (val ? 1u : 0u);
}

static uint32_t bb_read_bit(volatile void* addr, uint32_t bit)
{
  return *bb_alias_addr(addr, bit);
}

/* ------------------- Barrel rotate (CPU conditional) -------------- */
static uint32_t ror32(uint32_t x, unsigned n)
{
  n &= 31u;
  return (x >> n) | (x << (32u - n));
}

/* ------------------- LED helpers --------------- */
/* LED map: 0=P1.28, 1=P2.2, 2=P1.31, 3=P2.3, 4=P2.4 */
static void leds_init(void)
{
  LPC_GPIO1->FIODIR |= (1u<<28) | (1u<<31);
  LPC_GPIO2->FIODIR |= (1u<<2)  | (1u<<3) | (1u<<4);
}

static void leds_all_off(void)
{
  LPC_GPIO1->FIOCLR = (1u<<28) | (1u<<31);
  LPC_GPIO2->FIOCLR = (1u<<2)  | (1u<<3) | (1u<<4);
}

static void led_show(int idx)
{
  leds_all_off();  /* exactly one LED on */
  if      (idx == 0) LPC_GPIO1->FIOSET = (1u<<28);
  else if (idx == 1) LPC_GPIO2->FIOSET = (1u<<2);
  else if (idx == 2) LPC_GPIO1->FIOSET = (1u<<31);
  else if (idx == 3) LPC_GPIO2->FIOSET = (1u<<3);
  else if (idx == 4) LPC_GPIO2->FIOSET = (1u<<4);
}

/* ------------------- LCD helpers --------------- */
#define LCD_W  21   /* safe width for classic font */

static void pad_copy(unsigned char *dst, unsigned int maxw, const char *src)
{
  unsigned int i = 0;
  while (src && src[i] && i < maxw) { dst[i] = (unsigned char)src[i]; i++; }
  while (i < maxw) { dst[i++] = ' '; }
  dst[maxw] = 0;
}

static void lcd_line(unsigned int line, const char *txt)
{
  static unsigned char buf[LCD_W+1];
  pad_copy(buf, LCD_W, txt);
  GLCD_DisplayString(line, 0, 1, buf);
}

static void lcd_title(const char *msg)            { lcd_line(0, msg); }
static void lcd_active_text(const char *tn)
{
  static char tmp[32];
  unsigned int i = 0;
  const char *prefix = "Active: ";
  while (prefix[i]) { tmp[i] = prefix[i]; i++; }
  while (tn && *tn && i < sizeof(tmp)-1) { tmp[i++] = *tn++; }
  tmp[i] = 0;
  lcd_line(1, tmp);
}
static void lcd_status_line2(const char *txt)     { lcd_line(2, txt); }
static void lcd_line3(const char *txt)            { lcd_line(3, txt); }

/* ------------------- LCD lock + window --------------- */
static void lcd_lock(void)   { osMutexWait(lcd_mutex, osWaitForever); }
static void lcd_unlock(void) { osMutexRelease(lcd_mutex); }

static void hold_window(void)
{
  uint32_t t;
  for (t = 0; t < WINDOW_TICKS; ++t) { osDelay(1); }
}

/* ------------------- Threads & defs ------------------------------- */
static osThreadId tid_mem, tid_cpu, tid_app, tid_dev, tid_ui;

void Th_MemoryManagement     (void const *arg);
void Th_CPUManagement        (void const *arg);
void Th_ApplicationInterface (void const *arg);
void Th_DeviceManagement     (void const *arg);
void Th_UserInterface        (void const *arg);

osThreadDef(Th_MemoryManagement,     osPriorityNormal, 1, 0);
osThreadDef(Th_CPUManagement,        osPriorityNormal, 1, 0);
osThreadDef(Th_ApplicationInterface, osPriorityNormal, 1, 0);
osThreadDef(Th_DeviceManagement,     osPriorityNormal, 1, 0);
osThreadDef(Th_UserInterface,        osPriorityNormal, 1, 0);

/* ------------------- Init: LEDs/LCD + threads --------------------- */
int Init_Thread (void)
{
  leds_init();
  leds_all_off();

  GLCD_Init();
  GLCD_SetTextColor(White);
  GLCD_SetBackColor(Black);
  GLCD_Clear(Black);

  /* create mutexes before any thread can draw */
  log_mutex = osMutexCreate(osMutex(log_mutex));
  lcd_mutex = osMutexCreate(osMutex(lcd_mutex));

  lcd_lock();
  lcd_title("Q2 Demo: OS Roles");
  lcd_active_text("(waiting)");
  lcd_status_line2("logger ready");
  lcd_line3("                    ");
  lcd_unlock();

  /* Create all roles; RR equal priorities; ordering via signals */
  tid_mem = osThreadCreate(osThread(Th_MemoryManagement),     NULL);
  tid_cpu = osThreadCreate(osThread(Th_CPUManagement),        NULL);
  tid_app = osThreadCreate(osThread(Th_ApplicationInterface), NULL);
  tid_dev = osThreadCreate(osThread(Th_DeviceManagement),     NULL);
  tid_ui  = osThreadCreate(osThread(Th_UserInterface),        NULL);

  if (!tid_mem || !tid_cpu || !tid_app || !tid_dev || !tid_ui) {
    lcd_lock(); lcd_status_line2("ERR: thread create"); lcd_unlock();
    return -1;
  }
  return 0;
}

/* ========================= Implementations ========================= */

void Th_MemoryManagement(const void *arg)
{
  uint32_t b0; (void)arg;

  mem_access_counter++;

  led_show(0);
  lcd_lock();
  lcd_active_text("Memory");
  lcd_status_line2("bit-band ops...");

  /* Bit-band demo: set bit3, clear bit2, toggle bit0 */
  bb_write_bit((void*)&bb_word, 3u, 1u);
  bb_write_bit((void*)&bb_word, 2u, 0u);
  b0 = bb_read_bit((void*)&bb_word, 0u);
  bb_write_bit((void*)&bb_word, 0u, b0 ^ 1u);

  /* show bb_word hex */
  {
    char line[32];
    unsigned int i = 0;
    static const char H[16] = "0123456789ABCDEF";
    uint32_t v = bb_word;
    line[i++] = H[(v>>28)&0xF]; line[i++] = H[(v>>24)&0xF];
    line[i++] = H[(v>>20)&0xF]; line[i++] = H[(v>>16)&0xF];
    line[i++] = H[(v>>12)&0xF]; line[i++] = H[(v>>8)&0xF];
    line[i++] = H[(v>>4)&0xF];  line[i++] = H[(v>>0)&0xF];
    line[i]=0;
    lcd_line3(line);
  }

  hold_window();                 /* ~3 s spotlight (LCD locked) */
  lcd_status_line2("Memory done");
  lcd_unlock();

  /* hand off to CPU and wait the reply so ordering is visible */
  osSignalSet(tid_cpu, SIG_MM_TO_CPU);
  (void)osSignalWait(SIG_CPU_TO_MM, osWaitForever);

  osDelay(1);                    /* 1 tick per spec */
  osThreadTerminate(osThreadGetId());
}

void Th_CPUManagement(const void *arg)
{
  uint32_t x; unsigned rot; (void)arg;

  (void)osSignalWait(SIG_MM_TO_CPU, osWaitForever);

  cpu_access_counter++;

  led_show(1);
  lcd_lock();
  lcd_active_text("CPU");
  lcd_status_line2("rotate & reply");

  /* conditional rotate based on bb_word bit3 */
  x = (uint32_t)bb_word;
  rot = bb_read_bit((void*)&bb_word, 3u) ? 7u : 3u;
  x = ror32(x ^ 0xA5A5A5A5u, rot);
  (void)x;

  hold_window();
  lcd_status_line2("CPU done");
  lcd_unlock();

  osSignalSet(tid_mem, SIG_CPU_TO_MM);
  osThreadTerminate(osThreadGetId());
}

void Th_ApplicationInterface(const void *arg)
{
  (void)arg;

  /* Phase 1: present and write prefix (no waiting while locked) */
  led_show(2);
  lcd_lock();
  lcd_active_text("App");
  lcd_status_line2("write prefix...");
  osMutexWait(log_mutex, osWaitForever);
  strcpy((char*)logger, "App: begin -> ");
  osMutexRelease(log_mutex);
  lcd_line3((const char*)logger);
  lcd_unlock();

  /* Phase 2: coordinate with Device (no LCD lock held during wait) */
  osSignalSet(tid_dev, SIG_APP_READY);
  (void)osSignalWait(SIG_DEV_DONE, osWaitForever);

  /* Phase 3: show combined result and finish */
  app_counter++;
  led_show(2);
  lcd_lock();
  lcd_active_text("App");

  /* Wait for UI to finish before printing "App done" */
  (void)osSignalWait(SIG_UI_DONE, osWaitForever);

  lcd_line3((const char*)logger);    /* now contains both parts */
  hold_window();
  lcd_status_line2("App done");
  lcd_unlock();

  osDelay(1);
  osThreadTerminate(osThreadGetId());
}

void Th_DeviceManagement(const void *arg)
{
  (void)arg;

  (void)osSignalWait(SIG_APP_READY, osWaitForever);

  /* Device presents, appends, and signals back.
     It does not wait after signaling, so holding the LCD lock is safe. */
  led_show(3);
  lcd_lock();
  lcd_active_text("Device");
  lcd_status_line2("append & signal");

  osMutexWait(log_mutex, osWaitForever);
  strcat((char*)logger, "Device: close.");
  osMutexRelease(log_mutex);

  osSignalSet(tid_app, SIG_DEV_DONE);

  dev_counter++;
  lcd_line3((const char*)logger);
  hold_window();
  lcd_status_line2("Device done");
  lcd_unlock();

  osDelay(1);
  osThreadTerminate(osThreadGetId());
}

void Th_UserInterface(const void *arg)
{
  (void)arg;

  led_show(4);
  lcd_lock();
  lcd_active_text("User IF");
  lcd_status_line2("one-shot task");

  ui_user_count++;
  hold_window();
  lcd_status_line2("UI done");
  lcd_unlock();

  // Signal App to proceed
  osSignalSet(tid_app, SIG_UI_DONE);

  osDelay(1);
  osThreadTerminate(osThreadGetId());
}
