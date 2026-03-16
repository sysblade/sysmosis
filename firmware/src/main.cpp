#include <Arduino.h>
#include "ROController.h"
#include "WebInterface.h"
#include "config.h"

static ROController  controller;
static WebInterface* webIface = nullptr;

// =============================================================================
// Network task — Core 0, priority 1.
// ESPAsyncWebServer is interrupt-driven; this task only handles WiFi and
// reconnect checks. The web server callbacks fire independently via TCP/IP ISR.
// =============================================================================
static void networkTask(void* /*param*/) {
    webIface->begin();
    for (;;) {
        webIface->loop();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =============================================================================
// setup() — Core 1
// =============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Krosmosis v2.0 (Arduino/C++) ===");

    controller.begin();

    // Start network task on whichever core the loop task is NOT on.
    // ARDUINO_RUNNING_CORE defaults to 1 but is not guaranteed — derive it
    // at runtime so the two tasks always end up on different cores.
    const BaseType_t loopCore = xPortGetCoreID();  // core setup()/loop() runs on
    const BaseType_t netCore  = (loopCore == 0) ? 1 : 0;
    Serial.printf("[Main] Loop core: %d  Network core: %d\n",
                  (int)loopCore, (int)netCore);

    if (strlen(Config::WIFI_SSID) > 0) {
        webIface = new WebInterface(controller);
        xTaskCreatePinnedToCore(
            networkTask,
            "net",
            10240,    // stack — web + JSON needs headroom
            nullptr,
            1,        // same priority as loop task
            nullptr,
            netCore
        );
    }
}

// =============================================================================
// loop() — Core 1, driven by Arduino's loopTask.
// WDT is fed inside controller.update() before any other work.
// =============================================================================
void loop() {
    controller.update();
    delay(20);  // ~50 Hz
}
