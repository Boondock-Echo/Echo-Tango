#include "health_performance.h"
#include "health.h"
#include "main.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <atomic>

// Statistical CPU-idle estimate: sample the task interrupted by the FreeRTOS
// tick on each CPU. This avoids calibrating against a peak idle-hook rate and
// does not infer run time from gaps between callbacks. It is a SAMPLE of
// scheduler state, not an exact measurement of task execution time.
// Tick hooks run in ISR context. Keep them short and in IRAM.
namespace {
constexpr uint32_t kIntervalMs = 10000;
volatile uint32_t tickSamples[2] = {};
volatile uint32_t idleSamples[2] = {};
TaskHandle_t idleTask[2] = {};
bool initialized = false;
bool hooksOk[2] = {false, false};
uint32_t priorTicks[2] = {};
uint32_t priorIdle[2] = {};
uint32_t sampleTicks[2] = {};
uint32_t sampleIdle[2] = {};
uint32_t priorMs = 0;
uint32_t windowMs = 0;
bool ready = false;

void IRAM_ATTR sampleTick0() {
    ++tickSamples[0];
    if (xTaskGetCurrentTaskHandleForCPU(0) == idleTask[0]) ++idleSamples[0];
}
void IRAM_ATTR sampleTick1() {
    ++tickSamples[1];
    if (xTaskGetCurrentTaskHandleForCPU(1) == idleTask[1]) ++idleSamples[1];
}

void initHooks() {
    if (initialized) return;
    idleTask[0] = xTaskGetIdleTaskHandleForCPU(0);
    idleTask[1] = xTaskGetIdleTaskHandleForCPU(1);
    if (idleTask[0]) hooksOk[0] = (esp_register_freertos_tick_hook_for_cpu(sampleTick0, 0) == ESP_OK);
    if (idleTask[1]) hooksOk[1] = (esp_register_freertos_tick_hook_for_cpu(sampleTick1, 1) == ESP_OK);
    for (int i = 0; i < 2; i++) {
        priorTicks[i] = tickSamples[i];
        priorIdle[i] = idleSamples[i];
    }
    priorMs = millis();
    initialized = true;
}

void printTask(Stream& out, const char* name, const TaskHealthMetrics& t) {
    if (!t.isRunning) return;
    out.printf("  %-17s allocated=%lu B  min-free=%lu B  stack-used=%.1f%%\n",
        name, (unsigned long)t.allocatedStackSize,
        (unsigned long)t.minFreeStack, t.stackUtilizationPercent);
}
}

void health_performance_sample() {
    initHooks();
    const uint32_t now = millis();
    const uint32_t elapsed = now - priorMs;
    if (elapsed < kIntervalMs) return;
    windowMs = elapsed;
    for (int i = 0; i < 2; i++) {
        const uint32_t ticks = tickSamples[i];
        const uint32_t idle = idleSamples[i];
        sampleTicks[i] = ticks - priorTicks[i];
        sampleIdle[i] = idle - priorIdle[i];
        priorTicks[i] = ticks;
        priorIdle[i] = idle;
    }
    priorMs = now;
    ready = true;
}

void health_performance_print(Stream& out) {
    out.println("=== PERFORMANCE (FreeRTOS tick sampling) ===");
    if (!initialized) initHooks();
    if (!hooksOk[0] || !hooksOk[1]) {
        out.printf("Tick-hook installation: core0=%s core1=%s\n",
            hooksOk[0] ? "OK" : "FAILED", hooksOk[1] ? "OK" : "FAILED");
    }
    if (!ready) {
        out.println("Sampling: no completed 10-second window yet.");
    } else {
        out.printf("Sample duration: %lu ms\n", (unsigned long)windowMs);
        for (int i = 0; i < 2; i++) {
            if (!hooksOk[i] || sampleTicks[i] == 0) {
                out.printf("Core %d: no valid tick samples\n", i);
                continue;
            }
            const float idlePct = 100.0f * (float)sampleIdle[i] / (float)sampleTicks[i];
            out.printf("Core %d: idle samples %.1f%%; non-idle samples %.1f%%\n",
                       i, idlePct, 100.0f - idlePct);
            out.printf("Core %d raw: idle-ticks=%lu total-ticks=%lu\n", i,
                       (unsigned long)sampleIdle[i], (unsigned long)sampleTicks[i]);
        }
        out.println("Tick samples reflect the running task at periodic interrupt points; approximate, not exact CPU time.");
        out.println("No peak-baseline calibration and no idle-hook-gap thresholds.");
    }
    out.println("Task stack high-water marks (maintained by health.cpp):");
    const SystemHealthMetrics m = health_getHealthMetrics();
    printTask(out, "RecordTask", m.recordTaskHealth);
    printTask(out, "NetworkTask", m.uploadTaskHealth);
    printTask(out, "SerialTask", m.serialTaskHealth);
    printTask(out, "MaintenanceTask", m.maintenanceTaskHealth);
    printTask(out, "WebServer", m.webServerTaskHealth);
    out.printf("Heap: free=%lu B  minimum=%lu B  largest-block=%lu B\n",
               (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    out.printf("PSRAM: free=%lu B  total=%lu B\n",
               (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize());
}
