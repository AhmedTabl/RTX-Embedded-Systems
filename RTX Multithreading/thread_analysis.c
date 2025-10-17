/* COE718 Lab 3a — ANALYSIS VERSION*/

#include "cmsis_os.h"
#include <stdint.h>

/* ===== RR proof knobs ===== */
#define ACTIVATIONS_PER_TASK   150u   /* keep all three alive ~few seconds */
#define WORK_UNITS_PAINTER     28000u /* per-activation busy work */
#define WORK_UNITS_MORSE       28000u
#define WORK_UNITS_ROBOT       28000u

/* ===== Painter parameters ===== */
#ifndef LCD_TOTAL_COLUMNS
# define LCD_TOTAL_COLUMNS     30u
#endif

/* ===== Morse message ===== */
#ifndef MORSE_TMU
# define MORSE_TMU             "-  --  ..-"
#endif

/* ===== Robot waypoints ===== */
typedef struct { int8_t x, y; } wp_t;
#ifndef WP_COUNT
static const wp_t WAYPOINTS_LOCAL[] = { {2,0},{6,0},{6,3},{3,3},{0,3},{0,0} };
# define WAYPOINTS WAYPOINTS_LOCAL
# define WP_COUNT  (sizeof(WAYPOINTS_LOCAL)/sizeof(WAYPOINTS_LOCAL[0]))
#endif

/* ===== Watchable debug vars ===== */
volatile uint32_t g_active_tid = 0;   /* 1,2,3 = which thread currently running */
volatile uint32_t g_t1_acts    = 0;   /* activations done by Painter */
volatile uint32_t g_t2_acts    = 0;   /* activations done by Morse   */
volatile uint32_t g_t3_acts    = 0;   /* activations done by Robot   */
volatile uint32_t g_t1_bitmap  = 0;   /* low 32 bits: painted cols */
volatile uint32_t g_t2_idx     = 0;   /* morse index */
volatile int8_t   g_robot_x    = 0, g_robot_y = 0;
volatile uint32_t g_robot_wp_index = 0;
volatile uint8_t  g_t1_done    = 0, g_t2_done = 0, g_t3_done = 0;

/* ===== Threads ===== */
void Thread_Painter (void const *argument);
void Thread_Morse   (void const *argument);
void Thread_Robot   (void const *argument);

osThreadId tid_painter, tid_morse, tid_robot;
osThreadDef(Thread_Painter, osPriorityNormal, 1, 0);
osThreadDef(Thread_Morse,   osPriorityNormal, 1, 0);
osThreadDef(Thread_Robot,   osPriorityNormal, 1, 0);

static void do_busy_work(uint32_t units){
  volatile uint32_t acc = 0u;
  while (units--) {           
    acc ^= units;
    acc = (acc << 1) | (acc >> 31);
  }
}

int Init_Thread(void) {
  int ok = 1;
  tid_painter = osThreadCreate(osThread(Thread_Painter), NULL); if (!tid_painter) ok = 0;
  tid_morse   = osThreadCreate(osThread(Thread_Morse),   NULL); if (!tid_morse)   ok = 0;
  tid_robot   = osThreadCreate(osThread(Thread_Robot),   NULL); if (!tid_robot)   ok = 0;
  return ok ? 0 : -1;
}

/* --------------- Task A: Slice-Programmable Painter --------------- */
void Thread_Painter (void const *argument) {
  uint32_t act = 0;
  uint32_t col = 0;
  (void)argument;

  while (act < ACTIVATIONS_PER_TASK) {
    uint32_t i;
    g_active_tid = 1u;

    /* mark a few columns per activation */
    for (i = 0; i < 3u; ++i) {
      g_t1_bitmap |= (1u << (col & 31u));
      col++;
    }

    do_busy_work(WORK_UNITS_PAINTER);   /* keep READY; let RR do the preemption */
    g_t1_acts++;
    act++;
  }

  g_t1_done = 1u;
  osThreadTerminate(osThreadGetId());
}

/* -------------------- Task B: Morse “TMU” ------------------------- */
void Thread_Morse (void const *argument) {
  const char *msg = MORSE_TMU;
  uint32_t act = 0;
  uint32_t mlen = 0;
  (void)argument;

  /* compute length once */
  { const char *p = msg; while (*p) { mlen++; p++; } if (mlen == 0) mlen = 1; }

  while (act < ACTIVATIONS_PER_TASK) {
    char c;
    g_active_tid = 2u;

    c = msg[g_t2_idx];
    g_t2_idx++;
    if (g_t2_idx >= mlen) g_t2_idx = 0;

    if (c == '-')      do_busy_work(WORK_UNITS_MORSE + (WORK_UNITS_MORSE>>3));
    else               do_busy_work(WORK_UNITS_MORSE);

    g_t2_acts++;
    act++;
  }

  g_t2_done = 1u;
  osThreadTerminate(osThreadGetId());
}

/* --------------- Task C: Differential-Drive Waypoint Tracker ---------------- */
static int8_t sgn_i8(int8_t v){ return (int8_t)((v > 0) - (v < 0)); }

void Thread_Robot (void const *argument) {
  uint32_t act = 0;
  (void)argument;

  g_robot_x = 0; g_robot_y = 0; g_robot_wp_index = 0;

  while (act < ACTIVATIONS_PER_TASK) {
    int8_t tx, ty, dx, dy;

    g_active_tid = 3u;

    tx = WAYPOINTS[g_robot_wp_index % WP_COUNT].x;
    ty = WAYPOINTS[g_robot_wp_index % WP_COUNT].y;

    dx = (int8_t)(tx - g_robot_x);
    dy = (int8_t)(ty - g_robot_y);

    if (dx == 0 && dy == 0) {
      g_robot_wp_index++; /* reached this waypoint; move to next (wraps) */
    } else {
      if ((int16_t)dx * (int16_t)dx >= (int16_t)dy * (int16_t)dy) {
        g_robot_x = (int8_t)(g_robot_x + sgn_i8(dx));
      } else {
        g_robot_y = (int8_t)(g_robot_y + sgn_i8(dy));
      }
    }

    do_busy_work(WORK_UNITS_ROBOT);
    g_t3_acts++;
    act++;
  }

  g_t3_done = 1u;
  osThreadTerminate(osThreadGetId());
}

