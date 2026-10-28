#pragma once
#include <Arduino.h>

// Nonblocking; call frequently from MaintenanceTask (every loop iteration).
void health_performance_sample();
// Extends the existing Serial CLI `health ?` response.
void health_performance_print(Stream& out);
