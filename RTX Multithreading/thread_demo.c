#include "thread_tasks.h"   /* SLICES_TO_FINISH, LCD_TOTAL_COLUMNS, MORSE_TMU, WAYPOINTS... */
#include "cmsis_os.h"
#include "LPC17xx.h"
#include "GLCD.h"
#include <stdint.h>

/* ====== 2-second window per thread (RTX tick = 5 ms) ====== */
#ifndef RTX_TICK_US
# define RTX_TICK_US         5000u
#endif
#define WINDOW_TICKS         400u   /* 400 * 5 ms = 2000 ms = 2 s */

/* ---------- Fallback waypoints ---------- */
#ifndef WP_COUNT
typedef struct { int8_t x, y; } wp_t_local;
static const wp_t_local DFLT_WP[] = { {2,0},{6,0},{6,3},{3,3},{0,3},{0,0} };
#define WP_COUNT (sizeof(DFLT_WP)/sizeof(DFLT_WP[0]))
#define WP_AT(k) (DFLT_WP[(k)])
#else
#define WP_AT(k) (WAYPOINTS[(k)])
#endif

/* ---------- LED helpers ---------- */
/* 0 -> P1.28, 1 -> P2.2, 2 -> P1.31 */
static void leds_init(void) {
  LPC_GPIO1->FIODIR |= (1u<<28) | (1u<<31);
  LPC_GPIO2->FIODIR |= (1u<<2);
}
static void leds_all_off(void) {
  LPC_GPIO1->FIOCLR = (1u<<28) | (1u<<31);
  LPC_GPIO2->FIOCLR = (1u<<2);
}
static void led_show(int idx) {
  leds_all_off();                               /* exactly one LED on */
  if (idx == 0)      LPC_GPIO1->FIOSET = (1u<<28);
  else if (idx == 1) LPC_GPIO2->FIOSET = (1u<<2);
  else if (idx == 2) LPC_GPIO1->FIOSET = (1u<<31);
}

/* ---------- LCD helpers ---------- */
#define LCD_W  21  
static void pad_copy(unsigned char *dst, unsigned int maxw, const char *src){
  unsigned int i = 0;
  while (src && src[i] && i < maxw) { dst[i] = (unsigned char)src[i]; i++; }
  while (i < maxw) { dst[i++] = ' '; }
  dst[maxw] = 0;
}
static void lcd_line(unsigned int line, const char *txt){
  static unsigned char buf[LCD_W+1];
  pad_copy(buf, LCD_W, txt);
  GLCD_DisplayString(line, 0, 1, buf);
}
static void lcd_title(const char *msg){ lcd_line(0, msg); }
static void lcd_active_text(const char *tn){
  /* prints "Active: <name>" padded */
  static char tmp[32];
  unsigned int i = 0;
  const char *prefix = "Active: ";
  while (prefix[i]) { tmp[i] = prefix[i]; i++; }
  while (tn && *tn && i < sizeof(tmp)-1) { tmp[i++] = *tn++; }
  tmp[i] = 0;
  lcd_line(1, tmp);
}

/* progress bar to line 3, exact width */
static void lcd_bar_line3(unsigned int filled, unsigned int total){
  static char bar[32];
  unsigned int i, width = (LCD_W < 30u) ? LCD_W : 30u;
  if (total == 0) total = 1;
  if (filled > total) filled = total;
  /* compute chars to draw */
  {
    unsigned int n = (filled * width) / total;
    for (i = 0; i < n && i < width; i++) bar[i] = '#';
    while (i < width) bar[i++] = ' ';
    bar[i] = 0;
  }
  lcd_line(3, bar);
}

/* show small status on line 2, padded */
static void lcd_status_line2(const char *txt){ lcd_line(2, txt); }

/* plot dot for robot on text grid (lines 5..) */
static void lcd_plot_dot(uint32_t ln, uint32_t col){
  if (col < LCD_W) {
    GLCD_DisplayChar(5 + ln, col, 1, '.');
  }
}

/* ---------- tiny utils ---------- */
static int8_t sgn(int8_t v){ return (v>0) - (v<0); }

/* ---------- Threads & token (signals) ---------- */
void Thread_Painter (void const *argument);
void Thread_Morse   (void const *argument);
void Thread_Robot   (void const *argument);

osThreadId tid_painter, tid_morse, tid_robot;
static volatile uint8_t t1_done, t2_done, t3_done;

/* use default RTX stack (0) */
osThreadDef(Thread_Painter, osPriorityNormal, 1, 0);
osThreadDef(Thread_Morse,   osPriorityNormal, 1, 0);
osThreadDef(Thread_Robot,   osPriorityNormal, 1, 0);

#define SIG_TOKEN   (0x1u)

/* choose next alive thread in round-robin order */
static void pass_token_from(uint8_t from_id){
  /* from_id: 1=T1, 2=T2, 3=T3 */
  if (from_id == 1u) {
    if (!t2_done)      osSignalSet(tid_morse, SIG_TOKEN);
    else if (!t3_done) osSignalSet(tid_robot, SIG_TOKEN);
  } else if (from_id == 2u) {
    if (!t3_done)      osSignalSet(tid_robot, SIG_TOKEN);
    else if (!t1_done) osSignalSet(tid_painter, SIG_TOKEN);
  } else { /* from T3 */
    if (!t1_done)      osSignalSet(tid_painter, SIG_TOKEN);
    else if (!t2_done) osSignalSet(tid_morse, SIG_TOKEN);
  }
}

/* ---------- Init: create threads, give token to T1 ---------- */
int Init_Thread(void) {
  leds_init();
  leds_all_off();

  GLCD_Init();
  GLCD_SetTextColor(White);
  GLCD_SetBackColor(Black);
  GLCD_Clear(Black);

  t1_done = t2_done = t3_done = 0;

  lcd_title("Round-Robin Demo");
  lcd_active_text("(waiting)");
  lcd_status_line2("                   ");
  lcd_bar_line3(0, 1);

  tid_painter = osThreadCreate(osThread(Thread_Painter), NULL);
  tid_morse   = osThreadCreate(osThread(Thread_Morse),   NULL);
  tid_robot   = osThreadCreate(osThread(Thread_Robot),   NULL);

  if (!tid_painter) lcd_status_line2("ERR: T1 create");
  if (!tid_morse)   lcd_status_line2("ERR: T2 create");
  if (!tid_robot)   lcd_status_line2("ERR: T3 create");

  if (tid_painter && tid_morse && tid_robot) {
    osSignalSet(tid_painter, SIG_TOKEN);   /* start with T1 */
    return 0;
  }
  return -1;
}

/* =========================================================
   Task A — Slice-Programmable Pixel Painter
   One “activation” per 2-second window; finishes in SLICES_TO_FINISH.
   ========================================================= */
void Thread_Painter (void const *argument) {
  uint32_t slices, per_activation, painted = 0;
  (void)argument;

#ifndef LCD_TOTAL_COLUMNS
# define LCD_TOTAL_COLUMNS 30u
#endif
#ifndef SLICES_TO_FINISH
# define SLICES_TO_FINISH 3u
#endif

  slices = (SLICES_TO_FINISH < 1) ? 1 : SLICES_TO_FINISH;
  per_activation = (LCD_TOTAL_COLUMNS + slices - 1u) / slices;

  while (1) {
    osEvent ev = osSignalWait(SIG_TOKEN, osWaitForever);
    (void)ev;

    if (painted >= LCD_TOTAL_COLUMNS) {
      t1_done = 1u;
      lcd_status_line2("T1 Done");
      pass_token_from(1u);
      osThreadTerminate(osThreadGetId());
    }

    /* do one chunk */
    {
      uint32_t todo = per_activation, i;
      char label[24];
      if (painted + todo > LCD_TOTAL_COLUMNS) todo = LCD_TOTAL_COLUMNS - painted;

      led_show(0);
      lcd_active_text("T1");

      {
        unsigned int k = (painted + todo);
        unsigned int N = LCD_TOTAL_COLUMNS;
        label[0]='P';label[1]='a';label[2]='i';label[3]='n';label[4]='t';label[5]='e';label[6]='r';label[7]=':';label[8]=' ';
        label[9]  = (char)('0' + ((k*10u)/N));
        label[10] = (char)('/'); label[11]='1'; label[12]='0'; label[13]=0;
      }
      lcd_status_line2(label);

      /* show bar progress */
      lcd_bar_line3(painted + todo, LCD_TOTAL_COLUMNS);

      painted += todo;
    }

    /* hold token ~2s */
    {
      uint32_t t;
      for (t = 0; t < WINDOW_TICKS; t++) { osDelay(1); }
    }

    pass_token_from(1u);
  }
}

/* =========================================================
   Task B — Morse “TMU”
   One symbol per 2-second window;
   ========================================================= */
static void morse_symbol_consume(char c){
  if (c == '.')      osDelay(12);  /* ~60 ms */
  else if (c == '-') osDelay(24);  /* ~120 ms */
  else               osDelay(12);
}
static unsigned int morse_len(const char *s){
  unsigned int n=0; while (s && s[n]) n++; return n;
}

void Thread_Morse (void const *argument) {
  const char *p; unsigned int idx=0, total;
  (void)argument;
#ifndef MORSE_TMU
  #define MORSE_TMU "-  --  ..-"
#endif
  p = MORSE_TMU; total = morse_len(MORSE_TMU);

  while (1) {
    osSignalWait(SIG_TOKEN, osWaitForever);

    if (idx >= total) {
      t2_done = 1u;
      lcd_status_line2("T2 Done");
      pass_token_from(2u);
      osThreadTerminate(osThreadGetId());
    }

    led_show(1);
    lcd_active_text("T2");

    {
      char l2[32];
      const char *kind = (p[idx]=='.') ? "dot" : (p[idx]=='-') ? "dash" : "gap";
      l2[0]='M';l2[1]='o';l2[2]='r';l2[3]='s';l2[4]='e';l2[5]=':';l2[6]=' ';l2[7]=0;
      {
        unsigned int j=7, k=0;
        while (kind[k] && j<31){ l2[j++] = kind[k++]; }
        l2[j++]=' '; l2[j++]='(';
        l2[j++] = (char)('0' + ((idx+1)/10u));
        l2[j++] = (char)('0' + ((idx+1)%10u));
        l2[j++] = '/';
        l2[j++] = (char)('0' + (total/10u));
        l2[j++] = (char)('0' + (total%10u));
        l2[j++]=')'; l2[j]=0;
      }
      lcd_status_line2(l2);
    }

    /* line 3: progress bar over total symbols */
    lcd_bar_line3(idx+1, total);

    morse_symbol_consume(p[idx]);
    idx++;

    /* hold token ~2s */
    {
      uint32_t t;
      for (t = 0; t < WINDOW_TICKS; t++) { osDelay(1); }
    }

    pass_token_from(2u);
  }
}

/* =========================================================
   Task C — Differential-Drive Waypoint Tracker
   One grid step per 2-second window;
   ========================================================= */
void Thread_Robot (void const *argument) {
  int8_t x = 0, y = 0;
  uint32_t i = 0; (void)argument;

  while (1) {
    osSignalWait(SIG_TOKEN, osWaitForever);

    if (i >= WP_COUNT) {
      t3_done = 1u;
      lcd_status_line2("T3 Done");
      pass_token_from(3u);
      osThreadTerminate(osThreadGetId());
    }

    led_show(2);
    lcd_active_text("T3");

    /* one step toward current waypoint */
    {
      int8_t dx = (int8_t)(WP_AT(i).x - x);
      int8_t dy = (int8_t)(WP_AT(i).y - y);

      if (dx == 0 && dy == 0) {
        i++;  /* reached this waypoint */
      } else {
        if ((dx*dx) >= (dy*dy)) x = (int8_t)(x + sgn(dx));
        else                    y = (int8_t)(y + sgn(dy));
      }

      {
        char l2[32];
        int xi = (int)x, yi = (int)y;
        l2[0]='R';l2[1]='o';l2[2]='b';l2[3]='o';l2[4]='t';l2[5]=':';l2[6]=' ';l2[7]=0;
        {
          unsigned int j=7;
          l2[j++]='(';
          if (xi<0){ l2[j++]='-'; xi=-xi; }
          l2[j++] = (char)('0' + (xi/10)%10);
          l2[j++] = (char)('0' + (xi%10));
          l2[j++]=','; l2[j++]=' ';
          if (yi<0){ l2[j++]='-'; yi=-yi; }
          l2[j++] = (char)('0' + (yi/10)%10);
          l2[j++] = (char)('0' + (yi%10));
          l2[j++]=')'; l2[j++]=' ';
          /* "i/N" */
          {
            unsigned int ii = (i<100)? i:99;
            unsigned int NN = (WP_COUNT<100)? WP_COUNT:99;
            l2[j++] = (char)('0' + (ii/10));
            l2[j++] = (char)('0' + (ii%10));
            l2[j++] = '/';
            l2[j++] = (char)('0' + (NN/10));
            l2[j++] = (char)('0' + (NN%10));
            l2[j]=0;
          }
        }
        lcd_status_line2(l2);
      }

      /* plot dot for current pose */
      lcd_plot_dot((uint32_t)((y >= 0) ? y : 0), (uint32_t)((x >= 0) ? x : 0));
    }

    /* bar on line 3 = waypoint progress */
    lcd_bar_line3(i, WP_COUNT);

    /* hold token ~2s */
    {
      uint32_t t;
      for (t = 0; t < WINDOW_TICKS; t++) { osDelay(1); }
    }

    pass_token_from(3u);
  }
}

