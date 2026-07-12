#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "HX711.h"
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiManager.h>

#define DOUT 4
#define CLK 5

#define SERVO_PIN 18
#define BUZZER_PIN 12

// Pin tombol
#define BTN_LOOSEN 26
#define BTN_TIGHTEN 27
#define BTN_AUTO 25

LiquidCrystal_I2C lcd(0x27, 20, 4);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

HX711 scale;
Servo myServo;

// ================= AREA KALIBRASI MUTLAK (ABSOLUTE) =================
float calibration_factor = -200.0; 

// Angka dikunci permanen berdasarkan data fisik riil dari alatmu:
float RAW_MIN = 9000.0;   // Nilai mutlak saat tali KENDUR
float RAW_MAX = -6000.0;  // Nilai mutlak saat ditarik KENCANG / OVER

float BATAS_LOOSE = 10.0;        // Di bawah 10% = LOOSE (Motor mengencangkan)
float BATAS_OVERTENSION = 50.0;  // Di atas 50% = OVERTENSION (Motor mengendurkan + Alarm)
// =====================================================================

float tensionPercent = 0;
float currentRaw = 0.0;          
unsigned long lastScaleRead = 0; 

bool autoMode = false;

int servoStop = 90;
int servoTighten = 110;
int servoLoosen = 70;

unsigned long lastSend = 0;
unsigned long lastLCDUpdate = 0; 
String statusText = "NORMAL";

unsigned long autoBtnTimer = 0;
bool autoBtnActive = false;
int lastBeepSecond = -1; 

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

unsigned long autoActionTimer = 0;
int autoStepPhase = 0; 

// ================= FUNGSI MAPPING TEGANGAN YANG AMAN =================
float calculateTension(float raw, float min_val, float max_val) {
  if (max_val == min_val) return 0.0;
  
  float percentage = ((raw - min_val) / (max_val - min_val)) * 100.0;
  
  // Mengunci persentase agar rapi di rentang 0.0% - 100.0%
  if (percentage < 0.0) percentage = 0.0;
  if (percentage > 100.0) percentage = 100.0;
  
  return percentage;
}

// ================= WEBSOCKET =================
void notifyClients() {
  String data = String(tensionPercent) + "," + String(tensionPercent) + "," + String(tensionPercent);
  ws.textAll(data);
}

// ================= LCD =================
void updateLCD() {
  lcd.setCursor(0, 0);
  lcd.print("Tension:");
  if (tensionPercent < 10.0) lcd.print(" "); 
  lcd.print(tensionPercent, 1); 
  lcd.print("%    "); 

  lcd.setCursor(0, 1);
  lcd.print("Stat:");
  String statPad = statusText;
  while(statPad.length() < 13) statPad += " "; 
  lcd.print(statPad);

  lcd.setCursor(0, 2);
  lcd.print("Mode:");
  if(autoMode) lcd.print("AUTO        ");
  else lcd.print("MANUAL      ");

  lcd.setCursor(0, 3);
  String ipStr = WiFi.localIP().toString();
  while(ipStr.length() < 16) ipStr += " ";
  lcd.print(ipStr);
}

// ================= SERVO CONTROL =================
void servoStopFunc()    { myServo.write(servoStop); }
void servoTightenFunc() { myServo.write(servoTighten); }
void servoLoosenFunc()  { myServo.write(servoLoosen); }

// ================= BUZZER CONTROL =================
void handleBuzzer() {
  if (autoBtnActive && (millis() - autoBtnTimer > 500)) return; 

  bool isOvertension = (tensionPercent >= BATAS_OVERTENSION);
  bool isLoose = (tensionPercent <= BATAS_LOOSE);

  if (isOvertension) { 
    if (millis() - lastBuzzerToggle > 150) { 
      buzzerState = !buzzerState;
      if (buzzerState) tone(BUZZER_PIN, 1500); 
      else noTone(BUZZER_PIN);
      lastBuzzerToggle = millis();
    }
  } 
  else if (isLoose) { 
    if (millis() - lastBuzzerToggle > 600) { 
      buzzerState = !buzzerState;
      if (buzzerState) tone(BUZZER_PIN, 600);
      else noTone(BUZZER_PIN);
      lastBuzzerToggle = millis();
    }
  } 
  else { 
    noTone(BUZZER_PIN);
    buzzerState = false;
  }
}

// ================= BUTTON CONTROL =================
void handleButtons() {
  if(digitalRead(BTN_LOOSEN) == LOW) {
    autoMode = false;
    servoLoosenFunc();
    statusText = "LOOSENING";
  }
  else if(digitalRead(BTN_TIGHTEN) == LOW) {
    autoMode = false;
    servoTightenFunc();
    statusText = "TIGHTENING";
  }
  else if(digitalRead(BTN_AUTO) == LOW) {
    if(!autoBtnActive) {
      autoBtnTimer = millis(); 
      autoBtnActive = true;
      lastBeepSecond = -1; 
      autoMode = true; 
    }
    
    unsigned long holdDuration = millis() - autoBtnTimer;
    
    if(holdDuration > 3000) {
      servoStopFunc(); 
      noTone(BUZZER_PIN); 
      
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Resetting WiFi...");
      
      tone(BUZZER_PIN, 880, 200); delay(200);
      tone(BUZZER_PIN, 698, 200); delay(200);
      tone(BUZZER_PIN, 523, 400); delay(400);
      noTone(BUZZER_PIN);
      
      WiFiManager wm;
      wm.resetSettings(); 
      
      delay(1200);
      ESP.restart(); 
    }
    else if(holdDuration > 500) {
      int detikSisa = 3 - (holdDuration / 1000);
      statusText = "RESET IN " + String(detikSisa) + "s";
      if(detikSisa != lastBeepSecond && detikSisa > 0) {
        tone(BUZZER_PIN, 1000, 80); 
        lastBeepSecond = detikSisa;
      }
    }
  }
  else {
    autoBtnActive = false; 
    if(!autoMode && digitalRead(BTN_LOOSEN) == HIGH && digitalRead(BTN_TIGHTEN) == HIGH) {
      servoStopFunc();
    }
  }
}

// ================= AUTO CONTROL (STEP & WAIT) =================
void autoControl() {
  if(!autoMode) {
    autoStepPhase = 0; 
    return;
  }
  if(autoBtnActive && (millis() - autoBtnTimer > 500)) return; 

  unsigned long now = millis();

  if (autoStepPhase == 1) {
    if (now - autoActionTimer >= 200) { 
      servoStopFunc();                  
      autoStepPhase = 2;                
      autoActionTimer = now;            
    }
    if (tensionPercent >= BATAS_OVERTENSION) statusText = "AUTO LOOSEN";
    else statusText = "AUTO TIGHTEN";
  } 
  else if (autoStepPhase == 2) {
    statusText = "WAIT STABLE"; 
    if (now - autoActionTimer >= 600) { 
      autoStepPhase = 0;                
    }
  } 
  else {
    if(tensionPercent >= BATAS_OVERTENSION) {
      servoLoosenFunc();       
      statusText = "AUTO LOOSEN";
      autoStepPhase = 1;       
      autoActionTimer = now;
    }
    else if(tensionPercent <= BATAS_LOOSE) {
      servoTightenFunc();      
      statusText = "AUTO TIGHTEN";
      autoStepPhase = 1;       
      autoActionTimer = now;
    }
    else {
      servoStopFunc();         
      statusText = "STABLE";
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(BTN_LOOSEN, INPUT_PULLUP);
  pinMode(BTN_TIGHTEN, INPUT_PULLUP);
  pinMode(BTN_AUTO, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT); 

  lcd.begin(); 
  lcd.backlight();

  lcd.setCursor(0,0);
  lcd.print("Mulai Sensor...");
  
  scale.begin(DOUT, CLK);
  scale.set_scale(calibration_factor);
  
  // PENTING: scale.tare() SUDAH DIHAPUS TOTAL DI SINI!
  // Sensor sekarang membaca nilai mutlak (Absolute) dan tidak akan pernah bergeser lagi saat restart.

  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  servoStopFunc();

  WiFiManager wm;
  wm.setConfigPortalTimeout(120);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Connecting WiFi...");

  bool res = wm.autoConnect("SMS_AP"); 

  if(!res) {
    lcd.clear();
    lcd.print("WiFi Timeout/Fail");
    tone(BUZZER_PIN, 300, 1000); delay(1000);
    ESP.restart(); 
  }

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("WiFi Connected!");
  
  tone(BUZZER_PIN, 523, 150); delay(150);
  tone(BUZZER_PIN, 659, 150); delay(150);
  tone(BUZZER_PIN, 784, 300); delay(300);
  noTone(BUZZER_PIN);
  
  delay(1400); 
  lcd.clear(); 

  ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){});
  server.addHandler(&ws);
  server.begin();
}

// ================= LOOP =================
void loop() {
  if (scale.is_ready() && (millis() - lastScaleRead > 50)) {
    float raw_sample = scale.get_units(1);
    
    if (currentRaw == 0.0) {
      currentRaw = raw_sample;
    } else {
      currentRaw = (currentRaw * 0.7) + (raw_sample * 0.3); 
    }

    tensionPercent = calculateTension(currentRaw, RAW_MIN, RAW_MAX);

    Serial.print("RAW: ");
    Serial.print(currentRaw, 1);
    Serial.print(" | Min: ");
    Serial.print(RAW_MIN, 1);
    Serial.print(" | Max: ");
    Serial.print(RAW_MAX, 1);
    Serial.print(" => Tegangan: ");
    Serial.print(tensionPercent, 1);
    Serial.println("%");

    lastScaleRead = millis();
  }

  if (autoStepPhase == 0) {
    if(tensionPercent >= BATAS_OVERTENSION) statusText = "OVERTENSION";
    else if(tensionPercent <= BATAS_LOOSE) statusText = "LOOSE";
    else statusText = "NORMAL";
  }

  handleButtons();
  autoControl(); 
  handleBuzzer(); 

  if (millis() - lastLCDUpdate > 250) {
    updateLCD();
    lastLCDUpdate = millis();
  }

  if(millis() - lastSend > 1000) {
    notifyClients();
    lastSend = millis();
  }

  delay(10); 
}
