void setup() {
  pinMode(LED_BUILTIN, OUTPUT);   // Setup onboard LED
  Serial4.begin(115200);          // TX1 = Pin 1, RX1 = Pin 0
}

void loop() {
  // Send message
  Serial4.println("Hello, world!");

  // Flash LED
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);

  // Check for incoming data
  if (Serial4.available()) {
    char incoming = Serial4.read();
    if (incoming == '1') {
      Serial4.println("Received command: 1");
    }
  }

  delay(900);  // Remaining delay to make 1 second total
}
