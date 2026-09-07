#ifndef  __LED_H_
#define  __LED_H_

#include "driver/gpio.h"

#define LED_GPIO_PIN GPIO_NUM_48

/* Default RGB brightness, each channel ranges from 0 to 255. */
#define LED_RED   20
#define LED_GREEN 200
#define LED_BLUE  20

/* Task context only; initialize first and serialize calls from multiple tasks. */
#define LED_SET(on) LED_Set(!!(on))
#define LED_TOGGLE() LED_Toggle()

void LED_Init(void);
void LED_Set(int on);
void LED_Toggle(void);

#endif // ! 
