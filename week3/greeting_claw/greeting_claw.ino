#include <Servo.h>

Servo myServo;

const int trigPin = 9;
const int echoPin = 10;
const int servoPin = 6;

const int homeAngle = 90;      // center position, used when a wave finishes
const int waveAngleLow = 60;   // one side of the wave
const int waveAngleHigh = 120; // other side of the wave

const int cruiseAngleLow = 30;   // cruising sweep range (wider than the wave)
const int cruiseAngleHigh = 150;
const int cruiseStepDelay = 30;  // ms between each 1-degree cruising step -> slow, smooth motion

const int distanceThreshold = 30; // cm, someone "arrives" when closer than this

bool personPresent = false; // tracks whether someone is currently in range

int cruiseAngle = homeAngle;      // current angle while cruising
int cruiseDirection = 1;          // 1 = sweeping up, -1 = sweeping down
unsigned long lastCruiseStep = 0; // timestamp of the last cruising step

void setup() {
  myServo.attach(servoPin);
  myServo.write(homeAngle);
  cruiseAngle = homeAngle;

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  Serial.begin(9600);
}

void loop() {
  int distance = readDistanceCM();

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  if (distance < distanceThreshold && !personPresent) {
    // someone just arrived -> stop cruising and greet them
    personPresent = true;
    waveHello();
  } else if (distance >= distanceThreshold && personPresent) {
    // someone just left -> go back to cruising
    personPresent = false;
  }

  if (!personPresent) {
    cruiseStep(); // slowly sweep back and forth while nobody is around
  }

  delay(50); // short delay so distance checks stay frequent even while cruising
}

// One small step of the slow back-and-forth cruising sweep.
// Timed with millis() instead of delay() so it doesn't block distance sensing.
void cruiseStep() {
  unsigned long now = millis();
  if (now - lastCruiseStep < cruiseStepDelay) {
    return; // not time for the next step yet
  }
  lastCruiseStep = now;

  cruiseAngle += cruiseDirection;
  if (cruiseAngle >= cruiseAngleHigh) {
    cruiseAngle = cruiseAngleHigh;
    cruiseDirection = -1; // reverse direction at the top of the sweep
  } else if (cruiseAngle <= cruiseAngleLow) {
    cruiseAngle = cruiseAngleLow;
    cruiseDirection = 1; // reverse direction at the bottom of the sweep
  }

  myServo.write(cruiseAngle);
}

// Measure distance using the HC-SR04 sensor, returns distance in cm
int readDistanceCM() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout in case nothing is in range

  if (duration == 0) {
    return 400; // no echo received -> treat as "far away"
  }

  return duration * 0.034 / 2; // convert travel time to distance in cm
}

// Wave a few times to greet whoever just walked into range
void waveHello() {
  for (int i = 0; i < 3; i++) {
    myServo.write(waveAngleLow);
    delay(200);
    myServo.write(waveAngleHigh);
    delay(200);
  }
  myServo.write(homeAngle); // settle back to center while they're still around
  cruiseAngle = homeAngle;  // resume cruising from center once they leave
}
