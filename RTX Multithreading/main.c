#define osObjectsPublic
#include "osObjects.h"
#include "cmsis_os.h"

extern int Init_Thread(void);

extern void SystemInit(void);

int main(void) {
  SystemInit();                 /* clocks, PLL, SysTick base */
  osKernelInitialize();         /* init RTX kernel */
  if (Init_Thread() != 0) {
    /* If you want, spin here — but RTX will still start */
  }
  osKernelStart();              /* start scheduler: threads now run */
  /* idle forever */
  for (;;) { osDelay(1000); }
}
