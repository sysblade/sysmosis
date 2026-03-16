#include <Arduino.h>
#include "ROController.h"

// =============================================================================
// Single controller instance — lives for the lifetime of the program.
// =============================================================================
static ROController controller;

// =============================================================================
// setup() — runs once on boot
// =============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Krosmosis v2.0 (Arduino/C++) ===");
    controller.begin();
}

// =============================================================================
// loop() — driven by the Arduino runtime (FreeRTOS loopTask on Core 1).
//
// Phase 3 will add a second FreeRTOS task on Core 0 for WiFi + web serving.
// The delay here keeps the loop at roughly 50 Hz without burning CPU.
// The WDT is fed inside ROController::update() before any other work.
// =============================================================================
void loop() {
    controller.update();
    delay(20);  // ~50 Hz tick rate; adjust if tighter timing is needed
}
