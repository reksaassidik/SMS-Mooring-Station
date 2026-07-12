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

// Pin tombol sesuai update terbaru kamu
#define BTN_LOOSEN 26
#define BTN_TIGHTEN 27
#define BTN_AUTO 25

LiquidCrystal_I2C lcd(0x27, 20, 4);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

HX711 scale;
Servo myServo;

// ================= FIX KALIBRASI BERDASARKAN DATA REKSA =================
float calibration_factor = -200.0; 

float RAW_MIN = 2.0;      
float RAW_MAX = 1000.0;   

float BATAS_LOOSE = 10.0;        
float BATAS_OVERTENSION = 50.0;  
// ========================================================================

float tensionPercent = 0;
bool autoMode = false;

int servoStop = 90;
int servoTighten = 110;
int servoLoosen = 70;

unsigned long lastSend = 0;
String statusText = "NORMAL";

unsigned long autoBtnTimer = 0;
bool autoBtnActive = false;
int lastBeepSecond = -1; 

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// Variabel Kontrol Step Otomatis (Anti-Osilasi)
unsigned long autoActionTimer = 0;
int autoStepPhase = 0; // 0 = Cek data, 1 = Sedang bergerak, 2 = Diam tunggu sensor settle

// ================= WEBSOCKET =================
void notifyClients() {
  String data = String(tensionPercent) + "," + String(tensionPercent) + "," + String(tensionPercent);
  ws.textAll(data);
}

// ================= LCD =================
void updateLCD() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Tension:");
  lcd.print(tensionPercent, 1); 
  lcd.print("%");

  lcd.setCursor(0,1);
  lcd.print("Stat:");
  lcd.print(statusText);

  lcd.setCursor(0,2);
  lcd.print("Mode:");
  if(autoMode) lcd.print("AUTO");
  else lcd.print("MANUAL");

  lcd.setCursor(0,3);
  lcd.print(WiFi.localIP());
}

// ================= SERVO CONTROL =================
void servoStopFunc()   { myServo.write(servoStop); }
void servoTightenFunc() { myServo.write(servoTighten); }
void servoLoosenFunc()  { myServo.write(servoLoosen); }

// ================= BUZZER CONTROL =================
void handleBuzzer() {
  if (autoBtnActive && (millis() - autoBtnTimer > 500)) return; 

  if (statusText == "OVERTENSION" || statusText == "AUTO LOOSEN") { 
    if (millis() - lastBuzzerToggle > 150) { 
      buzzerState = !buzzerState;
      if (buzzerState) tone(BUZZER_PIN, 1500); 
      else noTone(BUZZER_PIN);
      lastBuzzerToggle = millis();
    }
  } 
  else if (statusText == "LOOSE" || statusText == "AUTO TIGHTEN") { 
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

// ================= AUTO CONTROL (METODE STEP & WAIT) =================
void autoControl() {
  if(!autoMode) {
    autoStepPhase = 0; // Reset phase jika mode auto mati
    return;
  }
  if(autoBtnActive && (millis() - autoBtnTimer > 500)) return; 

  unsigned long now = millis();

  if (autoStepPhase == 1) {
    // FASE 1: Servo sedang bergerak pelan. Beri durasi aktif sebentar saja.
    if (now - autoActionTimer >= 200) { // <-- Bergerak selama 200 milidetik saja
      servoStopFunc();                  // Langsung hentikan motor
      autoStepPhase = 2;                // Pindah ke fase diam/tunggu
      autoActionTimer = now;            // Reset timer untuk mengukur durasi diam
    }
    // Amankan tampilan text LCD agar tidak bentrok saat servo melangkah
    if (tensionPercent >= BATAS_OVERTENSION) statusText = "AUTO LOOSEN";
    else statusText = "AUTO TIGHTEN";
  } 
  else if (autoStepPhase == 2) {
    // FASE 2: Servo diam total. Beri waktu agar tegangan tali stabil & sensor selesai membaca data baru.
    statusText = "WAIT STABLE"; 
    if (now - autoActionTimer >= 600) { // <-- Jeda diam selama 600 milidetik sebelum cek ulang
      autoStepPhase = 0;                // Kembali ke fase cek kondisi tali
    }
  } 
  else {
    // FASE 0: Cek nilai riil sensor saat ini
    if(tensionPercent >= BATAS_OVERTENSION) {
      servoLoosenFunc();       // Mulai kendorkan
      statusText = "AUTO LOOSEN";
      autoStepPhase = 1;       // Picu masuk ke Fase 1 (Mulai melangkah)
      autoActionTimer = now;
    }
    else if(tensionPercent <= BATAS_LOOSE) {
      servoTightenFunc();      // Mulai kencangkan
      statusText = "AUTO TIGHTEN";
      autoStepPhase = 1;       // Picu masuk ke Fase 1 (Mulai melangkah)
      autoActionTimer = now;
    }
    else {
      servoStopFunc();         // Jika di rentang aman, biarkan servo diam total
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

  lcd.begin(); // Sesuai fungsi begin bawaan LCD kamu
  lcd.backlight();

  scale.begin(DOUT, CLK);
  scale.set_scale(calibration_factor);
  scale.tare();

  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  servoStopFunc();

  WiFiManager wm;
  wm.setConfigPortalTimeout(120);

  lcd.setCursor(0,0);
  lcd.print("Connecting WiFi...");

  bool res = wm.autoConnect("SMS_AP"); // Menggunakan SSID pilihanmu

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

  ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){});
  server.addHandler(&ws);
  server.begin();
}

// ================= LOOP =================
void loop() {
  float raw = abs(scale.get_units(5));

  Serial.print("Nilai RAW Sensor: ");
  Serial.println(raw);

  tensionPercent = ((raw - RAW_MIN) / (RAW_MAX - RAW_MIN)) * 100.0;
  
  if(tensionPercent < 0) tensionPercent = 0;
  if(tensionPercent > 100) tensionPercent = 100;

  if(tensionPercent >= BATAS_OVERTENSION) statusText = "OVERTENSION";
  else if(tensionPercent <= BATAS_LOOSE) statusText = "LOOSE";
  else statusText = "NORMAL";

  handleButtons();
  autoControl(); // Fungsi kontrol baru mengeksekusi logika step-by-step di sini
  handleBuzzer(); 
  updateLCD();

  if(millis() - lastSend > 1000) {
    notifyClients();
    lastSend = millis();
  }

  delay(50);
}
