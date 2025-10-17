#pragma once
#include "cmsis_os.h"
#include <stdint.h>

#define LCD_TOTAL_COLUMNS      30u
#define SLICES_TO_FINISH       3u   // Task A: finish in this many activations

// Task B (Morse for “TMU”)
static const char *MORSE_TMU = "-  --  ..-";

// ===== Task C (Robotics): Waypoints (grid units) =====
// Keep these small; each activation moves by exactly 1 grid step.
typedef struct { int8_t x, y; } wp_t;
static const wp_t WAYPOINTS[] = {
  {2,  0}, {6,  0}, {6, 3}, {3, 5}, {0, 5}, {0, 0} // a little loop
};
static const uint32_t WP_COUNT = sizeof(WAYPOINTS)/sizeof(WAYPOINTS[0]);

// Shared prototypes
int  Init_Thread(void);
void Thread_Painter (void const *argument);
void Thread_Morse   (void const *argument);
void Thread_Robot   (void const *argument);
