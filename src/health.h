#ifndef BOONDOCK_HEALTH_H
#define BOONDOCK_HEALTH_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "main.h"

// Global metrics (defined in health.cpp)
extern SystemHealthMetrics g_healthMetrics;

// Maintenance task function
void maintenanceTask(void *pvParameters);

// Update task health metrics for a specific task
void updateTaskHealthMetrics(TaskHealthMetrics& metrics, TaskHandle_t taskHandle, const char* taskName, uint32_t allocatedStackSize);

// Get task health metrics by task name
TaskHealthMetrics health_getTaskHealthMetrics(const char* taskName);

// Get mutex metrics
MutexMetrics health_getMutexMetrics();

// Get network quality metrics
NetworkQualityMetrics health_getNetworkQualityMetrics();

// Get storage health metrics
StorageHealthMetrics health_getStorageHealthMetrics();

// Get endpoint health metrics (for specific endpoint index)
EndpointHealthMetrics health_getEndpointHealthMetrics(size_t endpointIndex);

// Get system health metrics
SystemHealthMetrics health_getHealthMetrics();

// Reset all metrics
void health_resetMetrics();

// Initialize maintenance task health tracking
void health_initTaskHealth(TaskHealthMetrics& metrics, const char* taskName, TaskHandle_t handle, uint32_t stackSize);

#endif // BOONDOCK_HEALTH_H
