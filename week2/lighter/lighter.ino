int ldrPin = A0;
int led1 = 9;
int led2 = 10;
int led3 = 11;

void setup() {
  pinMode(led1, OUTPUT);
  pinMode(led2, OUTPUT);
  pinMode(led3, OUTPUT);
  Serial.begin(9600);
}

void loop() {
  int lightValue = analogRead(ldrPin);
  Serial.println(lightValue);

  // Your environment:
  // Normal brightness is around 300
  // Very dark is around 80

  // Convert the light value into darkness
  // Bright = 0
  // Dark = 255
  int darkness;

  if (lightValue >= 280) {
    darkness = 0;
  } else {
    darkness = map(lightValue, 280, 80, 0, 255);
    darkness = constrain(darkness, 0, 255);
  }

  // LED 1
  // Turns on first
  int brightness1 = map(darkness, 0, 85, 0, 255);
  brightness1 = constrain(brightness1, 0, 255);

  // LED 2
  // Turns on second
  int brightness2 = map(darkness, 85, 170, 0, 255);
  brightness2 = constrain(brightness2, 0, 255);

  // LED 3
  // Turns on last
  int brightness3 = map(darkness, 170, 255, 0, 255);
  brightness3 = constrain(brightness3, 0, 255);

  analogWrite(led1, brightness1);
  analogWrite(led2, brightness2);
  analogWrite(led3, brightness3);

  delay(20);
}