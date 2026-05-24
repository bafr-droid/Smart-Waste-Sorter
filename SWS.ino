#include "DFRobotDFPlayerMini.h"
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==================== KREDENSIAL WIFI & FIREBASE ====================
#define WIFI_SSID "Your_WIFI_SSID"
#define WIFI_PASSWORD "Your_WIFI_Password"
#define API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "YOUR_FIREBASE_DATABASE_URL"
#define USER_EMAIL "YOUR_FIREBASE_USER_EMAIL"
#define USER_PASSWORD "YOUR_FIREBASE_USER_PASSWORD"

// ==================== OLED ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ==================== DFPLAYER ====================
HardwareSerial mySoftwareSerial(1);
static const uint8_t PIN_MP3_TX = 26;
static const uint8_t PIN_MP3_RX = 27;
DFRobotDFPlayerMini player;

// ==================== SERVO ====================
const int servoPin1 = 14;
const int servoPin2 = 12;
const int freq = 50;
const int resolution = 16;
uint32_t dutyMin;
uint32_t dutyMax;

// ==================== SENSOR PEMILAH ====================
const int sensorIR = 34;        
const int sensorKapasitif = 25; 
const int sensorInduktif = 33;  

// ==================== SENSOR ULTRASONIK ====================
#define trigPin1  16  // HCSR1 - Organik
#define echoPin1  19
#define trigPin2  17  // HCSR2 - Logam
#define echoPin2  5
#define trigPin3  4   // HCSR3 - Anorganik
#define echoPin3  2

// ==================== VARIABEL SERVO & PEMILAH ====================
unsigned long servo1Time = 0;
unsigned long servo2Time = 0;
unsigned long cycleCompleteTime = 0;
bool servo1Done = false;
bool servo2Active = false;
bool systemReady = true;
int servo2State = 0;
const unsigned long CYCLE_COOLDOWN = 3000;

// Anti-bouncing IR
int lastIRState = HIGH;
unsigned long lastIRDebounceTime = 0;
const unsigned long IR_DEBOUNCE_DELAY = 200;
bool IRStable = false;

// ==================== VARIABEL ULTRASONIK & FIREBASE ====================
volatile int jarak1 = 0;           // HCSR1 - Organik
volatile int persentase1 = 0;      // Persentase Organik
volatile int jarak2 = 0;           // HCSR2 - Logam
volatile int persentase2 = 0;      // Persentase Logam
volatile int jarak3 = 0;           // HCSR3 - Anorganik
volatile int persentase3 = 0;      // Persentase Anorganik

// Sensor reading state
int induktifValue = LOW;
int kapasitifValue = LOW;
static bool waitingForKapasitif = false;
static unsigned long sensorReadTimer = 0;

// Counters untuk jenis sampah
volatile int countPlastik = 0;
volatile int countOrganik = 0;
volatile int countLogam = 0;

// Flags untuk upload
volatile bool needUploadCounter = false;
volatile bool needUploadUltra = false;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

// Task Handles
TaskHandle_t Task1;
TaskHandle_t Task2;

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);

  // Setup I2C untuk OLED
  Wire.begin();
  
  // Inisialisasi OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Sistem Pemilah"));
  display.println(F("Sampah Pintar"));
  display.println(F(""));
  display.println(F("Initializing..."));
  display.display();
  delay(2000);

  // Setup Sensor Pemilah
  pinMode(sensorIR, INPUT);
  pinMode(sensorKapasitif, INPUT);
  pinMode(sensorInduktif, INPUT);

  // Setup Sensor Ultrasonik
  pinMode(trigPin1, OUTPUT);
  pinMode(echoPin1, INPUT);
  pinMode(trigPin2, OUTPUT);
  pinMode(echoPin2, INPUT);

  // Setup Servo
  ledcAttach(servoPin1, freq, resolution);
  ledcAttach(servoPin2, freq, resolution);
  dutyMin = 65535 * 0.025;
  dutyMax = 65535 * 0.125;
  setServoAngle(servoPin2, 100);

  Serial.println("\n=================================");
  Serial.println("Sistem Pemilah Sampah Pintar");
  Serial.println("Dual Core ESP32");
  Serial.println("Sequential Sensor Reading");
  Serial.println("=================================");

  // DFPlayer
  mySoftwareSerial.begin(9600, SERIAL_8N1, PIN_MP3_RX, PIN_MP3_TX);
  Serial.println("Inisialisasi DFPlayer...");
  
  if (!player.begin(mySoftwareSerial)) {
    Serial.println("DFPlayer error! Sistem tetap berjalan.");
  } else {
    Serial.println("DFPlayer Mini berhasil!");
    player.volume(30);
    delay(100);
    player.play(1);
    delay(6500);
  }

  // ==================== CREATE TASKS ====================
  xTaskCreatePinnedToCore(
    firebaseTask,
    "Firebase Task",
    10000,
    NULL,
    1,
    &Task2,
    0
  );

  xTaskCreatePinnedToCore(
    pemilahTask,
    "Pemilah Task",
    10000,
    NULL,
    1,
    &Task1,
    1
  );

  Serial.println("=================================");
  Serial.println("Dual Core Tasks Created!");
  Serial.println("Core 0: Firebase & WiFi");
  Serial.println("Core 1: Pemilahan + OLED");
  Serial.println("=================================\n");
}

// ==================== LOOP KOSONG ====================
void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// ==================== TASK 1: PEMILAH SAMPAH + OLED (CORE 1) ====================
void pemilahTask(void * parameter) {
  Serial.println("Task Pemilah dimulai di Core 1");
  
  for(;;) {
    pemilahSampahSequential();
    kontrolServo2();
    bacaUltrasonik();
    updateOLED();
    vTaskDelay(1 / portTICK_PERIOD_MS); // Dikurangi jadi 1ms untuk lebih cepat
  }
}

// ==================== TASK 2: FIREBASE & WIFI (CORE 0) ====================
void firebaseTask(void * parameter) {
  Serial.println("Task Firebase dimulai di Core 0");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(300 / portTICK_PERIOD_MS);
  }
  Serial.println();
  Serial.println("WiFi Terhubung!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(1000);

  for(;;) {
    if (Firebase.ready() && !signupOK) {
      signupOK = true;
      Serial.println("Firebase Ready!");
    }

    if (needUploadCounter && signupOK) {
      Firebase.RTDB.setInt(&fbdo, "/statistik/plastik", countPlastik);
      Firebase.RTDB.setInt(&fbdo, "/statistik/organik", countOrganik);
      Firebase.RTDB.setInt(&fbdo, "/statistik/logam", countLogam);
      
      Serial.println("--- STATISTIK TERUPDATE ---");
      Serial.print("Plastik: "); Serial.println(countPlastik);
      Serial.print("Organik: "); Serial.println(countOrganik);
      Serial.print("Logam: "); Serial.println(countLogam);
      
      needUploadCounter = false;
    }

    if (needUploadUltra && signupOK) {
      Firebase.RTDB.setInt(&fbdo, "/tong_organik/persen_isi", persentase1);
      Firebase.RTDB.setInt(&fbdo, "/tong_organik/jarak_cm", jarak1);
      Firebase.RTDB.setInt(&fbdo, "/tong_logam/persen_isi", persentase2);
      Firebase.RTDB.setInt(&fbdo, "/tong_logam/jarak_cm", jarak2);
      
      Serial.println("--- MONITORING TERUPDATE ---");
      Serial.print("Organik: "); Serial.print(jarak1); 
      Serial.print("cm ("); Serial.print(persentase1); Serial.println("%)");
      Serial.print("Logam: "); Serial.print(jarak2); 
      Serial.print("cm ("); Serial.print(persentase2); Serial.println("%)");
      
      needUploadUltra = false;
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ==================== FUNGSI PEMILAH SAMPAH - OPTIMIZED FAST ====================
void pemilahSampahSequential() {
  
  int currentIR = digitalRead(sensorIR);
  unsigned long currentTime = millis();

  // Debouncing IR
  if (currentIR != lastIRState) {
    lastIRDebounceTime = currentTime;
    IRStable = false;
  }
  if ((currentTime - lastIRDebounceTime) > IR_DEBOUNCE_DELAY) {
    IRStable = true;
  }
  lastIRState = currentIR;

  // Cek cooldown
  if (!systemReady && (currentTime - cycleCompleteTime >= CYCLE_COOLDOWN)) {
    systemReady = true;
    waitingForKapasitif = false;
    Serial.println(">>> Sistem siap menerima sampah baru! <<<\n");
  }

  // ===== PROSES TUNGGU 500MS =====
  if (waitingForKapasitif) {
    if (millis() - sensorReadTimer >= 500) {
      int kapasitifValue = digitalRead(sensorKapasitif);
      waitingForKapasitif = false;
      
      Serial.print("  [2] Kapasitif: ");
      Serial.println(kapasitifValue == HIGH ? "HIGH" : "LOW");
      Serial.print("  [3] IR: ");
      Serial.println(currentIR == LOW ? "DETECTED" : "CLEAR");
      
      bool detected = false;
      
      // LOGAM
      if (kapasitifValue == HIGH && induktifValue == HIGH) {
        Serial.println("\n=================================");
        Serial.println(">>> HASIL: LOGAM");
        Serial.println("=================================");
        setServoAngle(servoPin1, 55);
        delay(1000);
        servo1Done = true;
        player.play(5);
        servo1Time = millis();
        detected = true;
        systemReady = false;
        countLogam++;
        needUploadCounter = true;
      }
      // ORGANIK
      else if (kapasitifValue == HIGH && induktifValue == LOW) {
        Serial.println("\n=================================");
        Serial.println(">>> HASIL: ORGANIK");
        Serial.println("=================================");
        setServoAngle(servoPin1, 145);
        delay(1000);
        servo1Done = true;
        player.play(2);
        servo1Time = millis();
        detected = true;
        systemReady = false;
        countOrganik++;
        needUploadCounter = true;
      }
      // PLASTIK/CAMPURAN
      else if (kapasitifValue == LOW && induktifValue == LOW && currentIR == LOW && IRStable) {
        Serial.println("\n=================================");
        Serial.println(">>> HASIL: PLASTIK/CAMPURAN");
        Serial.println("=================================");
        setServoAngle(servoPin1, 100);
        delay(1000);
        servo1Done = true;
        player.play(3);
        servo1Time = millis();
        detected = true;
        systemReady = false;
        countPlastik++;
        needUploadCounter = true;
      }
      
      if (detected) {
        delay(500);
      }
    }
    return;
  }

  // ===== DETEKSI OBJEK BARU =====
  if (systemReady && !servo1Done && !servo2Active && !waitingForKapasitif) {
    int quickInduktif = digitalRead(sensorInduktif);
    
    if (quickInduktif == HIGH || currentIR == LOW) {
      Serial.println(">>> Objek terdeteksi! Memulai pembacaan...");
      
      induktifValue = digitalRead(sensorInduktif);
      Serial.print("  [1] Induktif: ");
      Serial.println(induktifValue == HIGH ? "HIGH" : "LOW");
      
      sensorReadTimer = millis();
      waitingForKapasitif = true;
      
      Serial.println("  >>> Menunggu 500ms...");
    }
  }

  // Trigger servo2 setelah 3 detik
  if (servo1Done && !servo2Active) {
    unsigned long currentMillis = millis();
    if (currentMillis - servo1Time >= 3000) {
      Serial.println(">>> Servo2 mulai bergerak...");
      setServoAngle(servoPin2, 160);
      servo2Active = true;
      servo2State = 1;
      servo2Time = millis();
      servo1Done = false;
    }
  }
}

// ==================== FUNGSI UPDATE OLED - 3 JENIS SAMPAH ====================
void updateOLED() {
  static unsigned long lastOLEDUpdate = 0;
  const long oledInterval = 500;
  
  if (millis() - lastOLEDUpdate >= oledInterval) {
    lastOLEDUpdate = millis();
    
    display.clearDisplay();
    
    // Header
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("MONITORING SAMPAH"));
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);
    
    // ORGANIK (HCSR1)
    display.setTextSize(1);
    display.setCursor(0, 12);
    display.print(F("Organik: "));
    display.print(persentase1);
    display.print(F("%"));
    
    // PLASTIK/CAMPURAN
    display.setTextSize(1);
    display.setCursor(0, 21);
    display.print(F("Plastik: "));
    display.print(F("N/A"));
    
    // LOGAM (HCSR2)
    display.setTextSize(1);
    display.setCursor(0, 30);
    display.print(F("Logam: "));
    display.print(persentase2);
    display.print(F("%"));
    
    // ANORGANIK (HCSR3)
    display.setTextSize(1);
    display.setCursor(0, 39);
    display.print(F("Anorganik: "));
    display.print(persentase3);
    display.print(F("%"));
    
    // Garis pembatas
    display.drawLine(0, 48, 128, 48, SSD1306_WHITE);
    
    // Status
    display.setTextSize(1);
    display.setCursor(0, 51);
    display.print(F("Status: "));
    if (systemReady) {
      display.print(F("SIAP"));
    } else {
      display.print(F("PROSES"));
    }
    
    // Statistik counter
    display.setCursor(0, 60);
    display.setTextSize(1);
    display.print(F("O:"));
    display.print(countOrganik);
    display.print(F(" P:"));
    display.print(countPlastik);
    display.print(F(" L:"));
    display.print(countLogam);
    
    display.display();
  }
}

// ==================== FUNGSI KONTROL SERVO 2 ====================
void kontrolServo2() {
  if (servo2Active && servo2State == 1) {
    unsigned long currentMillis = millis();
    if (currentMillis - servo2Time >= 1000) {
      setServoAngle(servoPin2, 100);
      servo2Active = false;
      servo2State = 0;
      cycleCompleteTime = millis();
      Serial.println(">>> Siklus selesai. Menunggu cooldown...\n");
    }
  }
}

// ==================== FUNGSI BACA ULTRASONIK ====================
void bacaUltrasonik() {
  static unsigned long previousMillis = 0;
  const long interval = 1000;
  
  if (millis() - previousMillis >= interval) {
    previousMillis = millis();

    // HCSR1 - ORGANIK
    digitalWrite(trigPin1, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin1, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin1, LOW);
    long duration1 = pulseIn(echoPin1, HIGH);
    jarak1 = duration1 * 0.034 / 2;

    if (jarak1 > 0 && jarak1 < 400) { 
      int jarakClamped1 = constrain(jarak1, 12, 25);
      persentase1 = map(jarakClamped1, 25, 12, 0, 100);
    }

    delay(50);

    // HCSR2 - LOGAM
    digitalWrite(trigPin2, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin2, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin2, LOW);
    long duration2 = pulseIn(echoPin2, HIGH);
    jarak2 = duration2 * 0.034 / 2;

    if (jarak2 > 0 && jarak2 < 400) { 
      int jarakClamped2 = constrain(jarak2, 12, 25);
      persentase2 = map(jarakClamped2, 25, 12, 0, 100);
    }

    delay(50);

    // HCSR3 - ANORGANIK
    digitalWrite(trigPin3, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin3, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin3, LOW);
    long duration3 = pulseIn(echoPin3, HIGH);
    jarak3 = duration3 * 0.034 / 2;

    if (jarak3 > 0 && jarak3 < 400) { 
      int jarakClamped3 = constrain(jarak3, 12, 25);
      persentase3 = map(jarakClamped3, 25, 12, 0, 100);
    }

    needUploadUltra = true;
  }
}

// ==================== FUNGSI SET SERVO ====================
void setServoAngle(int pin, int angle) {
  uint32_t duty = map(angle, 0, 180, dutyMin, dutyMax);
  ledcWrite(pin, duty);
  Serial.print("Servo Pin ");
  Serial.print(pin);
  Serial.print(" -> ");
  Serial.print(angle);
  Serial.println("°");
}

// ==================== TOKEN STATUS CALLBACK ====================
void tokenStatusCallback(TokenInfo info) {
  if (info.status == token_status_ready) {
    Serial.println(F("Firebase token refreshed successfully"));
  }
  else if (info.status == token_status_error) {
    Serial.print(F("Firebase token error: "));
    Serial.println(info.error.message);
  }
}