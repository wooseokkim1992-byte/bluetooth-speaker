#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>

typedef struct _timer_obj_t{
  hw_timer_t *timer;
  SemaphoreHandle_t tickSignal;
}timer_obj_t;

bool init_timer(timer_obj_t *obj);
