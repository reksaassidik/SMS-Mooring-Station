#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "HX711.h"
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define DOUT 4
#define CLK 5

#define SERVO_PIN 18

#define BTN_LOOSEN 32
#define BTN_TIGHTEN 33
#define BTN_AUTO 25

LiquidCrystal_I2C lcd(0x27, 20, 4);

const char* ssid = "Robotik Juara";
const char* password = "juaraterus";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

HX711 scale;
Servo myServo;

float calibration_factor = -200;

float tensionPercent = 0;

bool autoMode = false;

int servoStop = 90;
int servoTighten = 110;
int servoLoosen = 70;

unsigned long lastSend = 0;

String statusText = "NORMAL";


// ================= WEBSOCKET =================

void notifyClients()
{
  String data = String(tensionPercent) + "," + String(tensionPercent) + "," + String(tensionPercent);
  ws.textAll(data);
}

// ================= LCD =================

void updateLCD()
{
  lcd.clear();

  lcd.setCursor(0,0);
  lcd.print("Tension:");
  lcd.print(tensionPercent,1);
  lcd.print("%");

  lcd.setCursor(0,1);
  lcd.print("Status:");
  lcd.print(statusText);

  lcd.setCursor(0,2);
  lcd.print("Mode:");
  if(autoMode) lcd.print("AUTO");
  else lcd.print("MANUAL");

  lcd.setCursor(0,3);
  lcd.print(WiFi.localIP());
}

// ================= SERVO CONTROL =================

void servoStopFunc()
{
  myServo.write(servoStop);
}

void servoTightenFunc()
{
  myServo.write(servoTighten);
}

void servoLoosenFunc()
{
  myServo.write(servoLoosen);
}

// ================= BUTTON CONTROL =================

void handleButtons()
{
  if(digitalRead(BTN_LOOSEN)==LOW)
  {
    autoMode=false;
    servoLoosenFunc();
    statusText="LOOSENING";
  }
  
  else if(digitalRead(BTN_TIGHTEN)==LOW)
  {
    autoMode=false;
    servoTightenFunc();
    statusText="TIGHTENING";
  }

  else if(digitalRead(BTN_AUTO)==LOW)
  {
    autoMode=true;
  }

  else
  {
    if(!autoMode)
    servoStopFunc();
  }
}

// ================= AUTO CONTROL =================

void autoControl()
{
  if(!autoMode) return;

  if(tensionPercent > 80)
  {
    servoLoosenFunc();
    statusText="AUTO LOOSEN";
  }
  
  else if(tensionPercent < 20)
  {
    servoTightenFunc();
    statusText="AUTO TIGHTEN";
  }

  else
  {
    servoStopFunc();
    statusText="STABLE";
  }
}


// ================= SETUP =================

void setup()
{
  Serial.begin(115200);

  pinMode(BTN_LOOSEN, INPUT_PULLUP);
  pinMode(BTN_TIGHTEN, INPUT_PULLUP);
  pinMode(BTN_AUTO, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  scale.begin(DOUT, CLK);
  scale.set_scale(calibration_factor);
  scale.tare();

  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN,500,2400);
  servoStopFunc();

  WiFi.begin(ssid,password);

  lcd.setCursor(0,0);
  lcd.print("Connecting WiFi");

  while(WiFi.status()!=WL_CONNECTED)
  {
    delay(500);
    lcd.print(".");
  }

  lcd.clear();
  lcd.print("Connected");

  ws.onEvent([](AsyncWebSocket * server,
                AsyncWebSocketClient * client,
                AwsEventType type,
                void * arg,
                uint8_t *data,
                size_t len){});

  server.addHandler(&ws);
  server.begin();

  Serial.println(WiFi.localIP());
}


// ================= LOOP =================

void loop()
{

  float raw = abs(scale.get_units(5));

  tensionPercent = map(raw,0,5000,0,100);
  tensionPercent = constrain(tensionPercent,0,100);

  // STATUS
  if(tensionPercent > 80) statusText="OVERTENSION";
  else if(tensionPercent < 10) statusText="LOOSE";
  else statusText="NORMAL";

  handleButtons();

  autoControl();

  updateLCD();

  if(millis() - lastSend > 1000)
  {
    notifyClients();
    lastSend = millis();
  }

  delay(50);
}
