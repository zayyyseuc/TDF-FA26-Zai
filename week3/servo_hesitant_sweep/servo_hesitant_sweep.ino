#include <Servo.h>

Servo myServo;
const int servoPin = 9;

int posA = 45;   // one end angle
int posB = 135;  // other end angle

void setup() {
  myServo.attach(servoPin);
  myServo.write(posA);
  randomSeed(analogRead(A0)); // use noise from an unconnected analog pin to seed randomness
}

void loop() {
  moveHesitant(posA, posB);
  pauseBetween();
  moveHesitant(posB, posA);
  pauseBetween();
}

// Move the servo from startAngle to endAngle, starting slow and speeding up (acceleration),
// with several chances to hesitate partway through
void moveHesitant(int startAngle, int endAngle) {
  int steps = 24;
  int direction = (endAngle > startAngle) ? 1 : -1;
  int totalDistance = abs(endAngle - startAngle);

  for (int i = 0; i <= steps; i++) {
    int angle = startAngle + direction * (i * totalDistance / steps);
    myServo.write(angle);

    // wider delay range -> the slow start and fast finish are both more noticeable
    int stepDelay = map(i, 0, steps, 80, 8);
    delay(stepDelay);

    // check for hesitation at a few points along the sweep instead of just the middle
    if ((i == steps / 3 || i == steps / 2 || i == (2 * steps) / 3) && random(0, 100) < 70) {
      hesitate(angle, direction);
    }
  }
}

// A hesitation beat: freeze, flinch backward a little (against the direction of travel),
// freeze again, then let the main loop continue forward. The backward flinch is what
// reads as "hesitating" rather than just "slowing down"
void hesitate(int currentAngle, int direction) {
  delay(random(300, 700)); // initial freeze

  int flinchAngle = currentAngle - direction * random(5, 15); // small step backward
  myServo.write(flinchAngle);
  delay(random(150, 300));

  myServo.write(currentAngle); // settle back to where it paused
  delay(random(250, 550)); // second freeze before continuing forward
}

// Pause between sweeps, longer and more varied so the overall rhythm feels uncertain
void pauseBetween() {
  int pauseTime = random(400, 2500);
  delay(pauseTime);
}
