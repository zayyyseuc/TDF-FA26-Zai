// HC-SR04 Ultrasonic Sensor + LED
// The closer an object is, the faster the LED blinks

const int trigPin = 9;
const int echoPin = 10;
const int ledPin = 13;

long duration;
int distance;

void setup() {
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(ledPin, OUTPUT);
  Serial.begin(9600);
}

void loop() {
  // Send a short pulse to trigger a measurement
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Read how long the echo pin stays HIGH (sound travel time in microseconds)
  duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout avoids getting stuck if no echo returns

  if (duration == 0) {
    // No echo received -> treat as "nothing in range"
    distance = 400; // arbitrary large value, out of normal sensing range
  } else {
    // Convert time to distance in cm
    // speed of sound ~0.034 cm/us, divide by 2 because the sound travels there and back
    distance = duration * 0.034 / 2;
  }

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  // Map distance to blink delay: closer = smaller delay = faster blinking
  // Tune the input range (2-100 cm) to match how you'll actually test it
  int blinkDelay = map(distance, 2, 100, 50, 500);
  blinkDelay = constrain(blinkDelay, 50, 500); // keep delay within a safe, visible range

  // One blink cycle at the calculated speed
  digitalWrite(ledPin, HIGH);
  delay(blinkDelay);
  digitalWrite(ledPin, LOW);
  delay(blinkDelay);
}
