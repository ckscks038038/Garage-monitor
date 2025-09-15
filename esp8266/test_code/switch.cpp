#include <Arduino.h>

#define SWITCH_PIN 12  // Change to your switch pin

void setup() {
	Serial.begin(115200);
	pinMode(SWITCH_PIN, INPUT_PULLUP); // Pin 12, switch to GND
	Serial.println("Switch detection started.");
}

void loop() {
	bool currentState = digitalRead(SWITCH_PIN);

	if (currentState == LOW) {
		Serial.println("ON");
	} else {
		Serial.println("OFF");
	}
	delay(100); // Adjust delay as needed for your application
}
