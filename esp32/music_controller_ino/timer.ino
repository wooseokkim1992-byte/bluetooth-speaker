#include <Arduino.h>
#include "timer_types.h"


void ARDUINO_ISR_ATTR timer_intr_func(void *param) {
  timer_obj_t *obj = static_cast<timer_obj_t *>(param);
  xSemaphoreGiveFromISR(obj->tickSignal, nullptr);
}

bool init_timer(timer_obj_t *obj) {
  if (obj == nullptr) return false;

  obj->timer = nullptr;
  obj->tickSignal = xSemaphoreCreateBinary();
  if (obj->tickSignal == nullptr) return false;

  obj->timer = timerBegin(10000);  // 10 kHz: 1 tick = 0.1 ms
  if (obj->timer == nullptr) {
    vSemaphoreDelete(obj->tickSignal);
    obj->tickSignal = nullptr;
    return false;
  }

  timerAttachInterruptArg(obj->timer, &timer_intr_func, obj);
  timerAlarm(obj->timer, 10000, true, 0);  // 10000 ticks = 1000 ms
  return true;
}
