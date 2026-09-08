import processing.serial.*;

Serial myPort;

int score = 0;
int combo = 0;

String state = "READY";
String feedback = "";

int startTime = 0;

// Arduino reaction time for each round is 1500 ms
float maxTimer = 1500;

// Duration for displaying PERFECT / MISS
int feedbackTime = 0;
int feedbackDuration = 500;


void setup() {

  size(900, 600);

  // Print all available serial ports for debugging
  println(Serial.list());

  // Your Arduino
  myPort = new Serial(
    this,
    "/dev/cu.usbmodem101",
    9600
  );

  // Trigger serialEvent() whenever Arduino sends a new line
  myPort.bufferUntil('\n');

  textAlign(CENTER, CENTER);
}


void draw() {

  background(10);

  // -------------------------
  // Automatically clear PERFECT / MISS
  // -------------------------

  if (!feedback.equals("")) {

    if (millis() - feedbackTime > feedbackDuration) {
      feedback = "";
    }
  }


  // -------------------------
  // TITLE
  // -------------------------

  fill(255);
  textAlign(CENTER, CENTER);
  textSize(28);

  text("LIGHT / SHADOW", width / 2, 55);


  // -------------------------
  // SCORE
  // -------------------------

  textAlign(LEFT, CENTER);

  fill(180);
  textSize(20);

  text("SCORE", 50, 110);

  fill(255);
  textSize(40);

  text(score, 50, 150);


  // -------------------------
  // COMBO
  // -------------------------

  textAlign(RIGHT, CENTER);

  fill(180);
  textSize(20);

  text("COMBO", width - 50, 110);

  fill(255);
  textSize(40);

  text(combo, width - 50, 150);


  // -------------------------
  // MAIN PROMPT
  // -------------------------

  textAlign(CENTER, CENTER);

  if (state.equals("GREEN")) {

    fill(120, 255, 150);

    textSize(55);
    text("KEEP LIGHT", width / 2, 300);

    fill(180);
    textSize(20);

    text(
      "Do not cover the sensor",
      width / 2,
      355
    );
  }

  else if (state.equals("BLUE")) {

    fill(100, 180, 255);

    textSize(55);
    text("COVER", width / 2, 300);

    fill(180);
    textSize(20);

    text(
      "Block the light sensor",
      width / 2,
      355
    );
  }

  else {

    fill(255);

    textSize(50);
    text(state, width / 2, 300);
  }


  // -------------------------
  // TIMER BAR
  // -------------------------

  if (
    state.equals("GREEN") ||
    state.equals("BLUE")
  ) {

    float elapsed = millis() - startTime;

    float progress =
      constrain(
        elapsed / maxTimer,
        0,
        1
      );

    float barWidth = 600;

    // Background bar
    fill(50);

    rect(
      width / 2 - barWidth / 2,
      420,
      barWidth,
      12
    );

    // Remaining time
    fill(255);

    rect(
      width / 2 - barWidth / 2,
      420,
      barWidth * (1 - progress),
      12
    );
  }


  // -------------------------
  // FEEDBACK
  // -------------------------

  if (feedback.equals("PERFECT")) {

    fill(255);

    textSize(45);

    text(
      "PERFECT!",
      width / 2,
      500
    );
  }

  else if (feedback.equals("MISS")) {

    fill(255, 80, 80);

    textSize(45);

    text(
      "MISS",
      width / 2,
      500
    );
  }
}


// -------------------------
// SERIAL INPUT
// -------------------------

void serialEvent(Serial myPort) {

  String message =
    myPort.readStringUntil('\n');

  if (message == null) {
    return;
  }

  message = trim(message);

  println(message);


  // -------------------------
  // GREEN
  // -------------------------

  if (message.equals("GREEN")) {

    state = "GREEN";

    feedback = "";

    startTime = millis();
  }


  // -------------------------
  // BLUE
  // -------------------------

  else if (message.equals("BLUE")) {

    state = "BLUE";

    feedback = "";

    startTime = millis();
  }


  // -------------------------
  // CORRECT
  // -------------------------

  else if (message.equals("CORRECT")) {

    score += 100;

    combo += 1;

    feedback = "PERFECT";

    feedbackTime = millis();
  }


  // -------------------------
  // WRONG
  // -------------------------

  else if (message.equals("WRONG")) {

    combo = 0;

    feedback = "MISS";

    feedbackTime = millis();
  }
}
