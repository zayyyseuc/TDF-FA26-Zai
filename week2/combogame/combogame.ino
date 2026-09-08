int ldrPin = A0;

int greenLED = 9;
int blueLED = 10;

int threshold = 180;

void setup() {
  pinMode(greenLED, OUTPUT);
  pinMode(blueLED, OUTPUT);

  Serial.begin(9600);

  randomSeed(analogRead(A1));
}

void loop() {

  int choice = random(0, 2);

  // generate a prompt
  if (choice == 0) {

    digitalWrite(greenLED, HIGH);
    digitalWrite(blueLED, LOW);

    Serial.println("GREEN");

  } else {

    digitalWrite(greenLED, LOW);
    digitalWrite(blueLED, HIGH);

    Serial.println("BLUE");
  }

  // waiting
  delay(1500);

  int lightValue = analogRead(ldrPin);

  bool covered = lightValue < threshold;

  bool correct = false;

  if (choice == 0 && covered == false) {
    correct = true;
  }

  if (choice == 1 && covered == true) {
    correct = true;
  }

  // check
  if (correct) {

    Serial.println("CORRECT");

    // success feedback
    digitalWrite(greenLED, HIGH);
    digitalWrite(blueLED, HIGH);

    delay(300);

  } else {

    Serial.println("WRONG");

    // failure feedback
    digitalWrite(greenLED, LOW);
    digitalWrite(blueLED, LOW);

    delay(2000);
  }
}