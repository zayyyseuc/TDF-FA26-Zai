//AI generated
#include <Servo.h>

Servo myServo;


// =====================================================
// SERVO PIN
// =====================================================

const int SERVO_PIN = 8;


// =====================================================
// SERVO VALUES
// =====================================================

// Fixed slow speed

const int LEFT_US = 1400;

const int STOP_US = 1500;

const int RIGHT_US = 1600;


// =====================================================
// SERIAL
// =====================================================

String inputString = "";


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(
    9600
  );


  myServo.attach(
    SERVO_PIN
  );


  // Stop on power-up
  myServo.writeMicroseconds(
    STOP_US
  );


  delay(
    500
  );
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  while (
    Serial.available() >
    0
  ) {

    char incomingChar =
      Serial.read();


    // =================================================
    // END OF COMMAND
    // =================================================

    if (
      incomingChar ==
      '\n'
    ) {

      if (
        inputString.length() >
        0
      ) {

        int command =
          inputString.toInt();


        // =============================================
        // Only allow three states
        // =============================================
        //
        // p5 should theoretically only send
        // 1400 / 1500 / 1600
        //
        // Double-check here for safety
        // =============================================


        if (
          command <
          1450
        ) {

          myServo.writeMicroseconds(
            LEFT_US
          );

        }


        else if (
          command >
          1550
        ) {

          myServo.writeMicroseconds(
            RIGHT_US
          );

        }


        else {

          myServo.writeMicroseconds(
            STOP_US
          );

        }
      }


      inputString =
        "";
    }


    // =================================================
    // READ DIGITS
    // =================================================

    else {

      if (
        isDigit(
          incomingChar
        )
      ) {

        inputString +=
          incomingChar;

      }
    }
  }
}
