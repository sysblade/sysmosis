#include <Arduino.h>
#include "ROController.h"
#include "WebInterface.h"
#include "config.h"

static ROController  controller;
static WebInterface* webIface = nullptr;

// =============================================================================
// Network task — pinned to the core opposite the loop task.
//
// Intentionally NOT subscribed to the task WDT:
//   - WiFi reconnects and OTA uploads can legitimately stall for seconds.
//   - The control task (loop) feeds the WDT independently; a hang there
//     still triggers a reset.
// =============================================================================
static void networkTask(void* /*param*/) {
    webIface->begin();

    Serial.printf("[Net] Stack HWM after begin: %u words free\n",
                  uxTaskGetStackHighWaterMark(NULL));

    for (;;) {
        webIface->loop();
        vTaskDelay(pdMS_TO_TICKS(10));  // 100 Hz poll keeps OTA responsive
    }
}

// =============================================================================
// setup() — runs on the loop task's core before loop() starts
// =============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Krosmosis v2.0 (Arduino/C++) ===");

    controller.begin();

    Serial.printf("[Main] Setup stack HWM: %u words free\n",
                  uxTaskGetStackHighWaterMark(NULL));

    // Pin the network task to whichever core setup()/loop() is NOT on.
    // ARDUINO_RUNNING_CORE defaults to 1 but is not guaranteed.
    const BaseType_t loopCore = xPortGetCoreID();
    const BaseType_t netCore  = (loopCore == 0) ? 1 : 0;
    Serial.printf("[Main] Loop core: %d  Network core: %d\n",
                  (int)loopCore, (int)netCore);

    if (strlen(Config::WIFI_SSID) > 0) {
        webIface = new WebInterface(controller);
        xTaskCreatePinnedToCore(
            networkTask,
            "net",
            10240,    // words — headroom for ArduinoOTA + ArduinoJson + TLS
            nullptr,
            1,
            nullptr,
            netCore
        );
    }
}

// =============================================================================
// loop() — control task, Core 1 (typically).
// WDT is fed at the top of controller.update() before any other work.
// =============================================================================
void loop() {
    controller.update();
    delay(20);  // ~50 Hz tick rate
}
