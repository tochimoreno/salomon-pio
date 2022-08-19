/* Salomon: Solar Monitor
*/

// macros
#define DEVICE                      "ESP8266"
#define WM_CONFIG_PORTAL_SSID       "Salomon"
#define WM_CONFIG_PORTAL_PASS       "ReySalomon"
#define ESP8266_DRD_USE_RTC         true
#define ESP_DRD_USE_LITTLEFS        false
#define ESP_DRD_USE_SPIFFS          false
#define ESP_DRD_USE_EEPROM          false
#define DOUBLERESETDETECTOR_DEBUG   false
#define DRD_TIMEOUT     20
#define DRD_ADDRESS     0
#define LED             2
#define STATUS_LED_INTERVAL 50

// Macros de la base de datos
#define INFLUXDB_URL    "http://ls.luxis.com.ar:8086"
#define INFLUXDB_ORG    "luxis"
#define INFLUXDB_BUCKET "dev"
#define INFLUXDB_TOKEN  "8Mo9YyFn6uE8RJ32OH4S5EwyCfeBAebmhhWkCzq35LG4ptNd3p30tGb66u9kYG6CPLIyhM0LhP9RtVvfXxJI0Q=="
#define DB_SENSOR_INTERVAL  120000         // en mS. 120_000 = 2 minutos

// Macros para el server NTP
#define TZ_INFO       "ART+3"
#define NTP_SERVER1   "south-america.pool.ntp.org"
#define NTP_SERVER2   "ar.pool.ntp.org"
#define NTP_SERVER3   "pool.ntp.org" 
#define NTP_UPDATE_INTERVAL 86400000    // 86_400_000 = 24hrs

// Includes
#include <Arduino.h>
#include <locale.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESP_DoubleResetDetector.h>
#include <WiFiClient.h>
#include <InfluxDbClient.h>
#include "SimplexNoise.h"
#include "salomon.h"

// Inicializadores
DoubleResetDetector*  drd;
WiFiManager           wm;
ESP8266WebServer      ws(80);
InfluxDBClient        client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
Point                 sensor("wifi_status");
Point                 inv("inv_status");
SimplexNoise          sn;

// Varialbes de estado
String sId = String(ESP.getChipId(), HEX);
bool stsWiFi = false;
bool stsWS   = false;
bool stsNTP  = false;
bool stsRTC  = false;
bool stsSD   = false;
bool stsDB   = false;
uint8_t stsLed = 0;
uint8_t stsBits = 0x80;

int  dbSensorLast = 0;         // Ultima llamada a la DB
int  timeSyncLast = 0;         // Ultima sync con el NTP
int  statusLedLast = 0;         // Ultima prendida del Led
time_t tnow;

// Variables para el generador de ruido
// Solo es un juguete del Tochi
double n;
float increase = 0.01;
//float x = map(random(0,100),0,100,0,1);
float x = random(0,1);
float y = 0;

/***********************************
 * Funciones
 * *********************************/
String EPOCH2ISO(time_t dt) {
  // Esto es solo para mostar la fechora en un formato lindo
  struct tm *lt = localtime(&dt);

  char strBuf[20];
  strftime(strBuf, sizeof strBuf, "%Y-%m-%d %X", lt);
  return strBuf;
}

/* --------------------------
 * Web Server functions
 * --------------------------
 */
void handleRoot() {
  time_t t = time(nullptr);
  
  char buf[20];
  sprintf(buf, "%llu", t);
  String dt; 
  dt = EPOCH2ISO(t);

  String page;
  page  = FPSTR(HEADER);
  page += FPSTR(STYLE);
  page += FPSTR(BODY);
  page += FPSTR(LOGO64);
  page += FPSTR(CONTENT);
  page += FPSTR(TABLE);
  page += FPSTR(END);

  page.replace("{id}", sId);
  page.replace("{t}", dt);

  ws.sendHeader("Content-Length", String(page.length()));
  ws.send(200, "text/html", page);
}

void handleAbout() {
  String page;

  page  = FPSTR(HEADER);
  page += FPSTR(STYLE);
  page += FPSTR(BODY);
  page += FPSTR(LOGO64);
  page += FPSTR(END);

  ws.sendHeader("Content-Length", String(page.length()));
  ws.send(200, "text/html", page);  
}

void handleNotFound(){
  ws.sendHeader("Location","/");        // Add a header to respond with a new location for the browser to go to the home page again
  ws.send(303);                         // Send it back to the browser with an HTTP status 303 (See Other) to redirect
//  server.send(404, "text/plain", "404: Not found"); // Send HTTP status 404 (Not Found) when there's no handler for the URI in the request
}

/* --------------------------
 * DB functions
 * --------------------------
 */
void dbUpdateSensor() {

  if (!stsDB) {
    Serial.println("\nDB");

    if (client.validateConnection()) {
      Serial.print("  Conectado a la DB: ");
      Serial.println(client.getServerUrl());
      stsDB = true;
    } else {
      Serial.print("  Fallo en conectar DB: ");
      Serial.println(client.getLastErrorMessage());
      stsDB = false;
      // activar logeo en SD cuando no hay DB
    }
    return;
  }

// Solo voy a seguir si se cumplio el tiempo de update
  if (!((millis() - dbSensorLast >= DB_SENSOR_INTERVAL)
    || dbSensorLast == 0)) {
    return;
  }

//------------------
//---- SENSOR ----//
//------------------
  // Veo la hora, para ponerla en todos los puntos igual
  tnow = time(nullptr);
  Serial.println(EPOCH2ISO(tnow));

  sensor.clearFields();
  sensor.addField("rssi", WiFi.RSSI());
  sensor.setTime(tnow);
  Serial.println("  " + client.pointToLineProtocol(sensor));

  // Write point
  if (!client.writePoint(sensor)) {
    Serial.print("  Fallo en escritura: ");
    Serial.println(client.getLastErrorMessage());
  }

//--------------------
//---- INVERSOR ----//
//--------------------
  n = sn.noise(x, y);

  inv.clearFields();
  inv.addField("noise",  n);
  inv.addField("offset", y);
  inv.setTime(tnow);

  Serial.println("  " + client.pointToLineProtocol(inv));

  // Preparo para el proximo valor de ruido
    y += increase;
  if (y > 6.0 || y < -6.0) {
    x += increase * 2;
    increase *= -1;
  }

  if (!client.writePoint(inv)) {
    Serial.print("  Fallo en escritura: ");
    Serial.println(client.getLastErrorMessage());
  }

 if (client.isBufferFull()) {
    // Write all remaining points to db
    Serial.print("  flushBuffer");
    client.flushBuffer();
 }


  Serial.println("-");
  dbSensorLast = millis();
}

/***********************************
 * MAIN
 * *********************************/
void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  digitalWrite(LED,LOW);
  WiFi.mode(WIFI_STA);
  drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
  sId.toUpperCase(); 
  sensor.addTag("device", sId);
  sensor.addTag("SSID", WiFi.SSID());
  inv.addTag("device", sId);
  delay(500);
  digitalWrite(LED,HIGH);
  delay(500);
  digitalWrite(LED,LOW);

//  while (!Serial) {  }
  delay(3000);
  Serial.println("\n\n**** Ini ****");

  if (drd->detectDoubleReset()) 
  {
    //Serial.println("  DRst detectado");

      wm.setConfigPortalTimeout(120);
      if (!wm.startConfigPortal(WM_CONFIG_PORTAL_SSID, WM_CONFIG_PORTAL_PASS)) {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        //reset and try again, or maybe put it to deep sleep
        ESP.restart();
        delay(5000);
      }
  }


// Inicio WifiManager
  Serial.println("WiFiMan");
  stsWiFi = wm.autoConnect(WM_CONFIG_PORTAL_SSID, WM_CONFIG_PORTAL_PASS);

  if(stsWiFi) {
    Serial.println("\nConectado a WiFi");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Failed to connect");
    ESP.restart();
    while (1) delay(50);
  }

  // OTA
  ArduinoOTA.setPort(8266);
//  ArduinoOTA.setPassword("admin");
  ArduinoOTA.begin();
  Serial.println("OTA Ini");

// NTP
  if(stsWiFi) {
    Serial.println("\nSincronizando Time");

    // Uso el timeSync de InfluxDB
    timeSync(TZ_INFO, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
    stsNTP = true;    // Lo supongo correcto por ahora.
    timeSyncLast = millis();
    Serial.println("  NTP OK");
  }

  // Acá Inicializar el RTC

  // Acá Inicializar la SD

  digitalWrite(LED, LOW); 

  ws.on("/", handleRoot);               // Call the 'handleRoot' function when a client requests URI "/"
  ws.on("/about", handleAbout);
  ws.onNotFound(handleNotFound);        // When a client requests an unknown URI (i.e. something other than "/"), call function "handleNotFound"
  ws.begin();                           // Actually start the server
  stsWS = true;
  Serial.println("\nHTTP server started");

  digitalWrite(LED, HIGH); 

  //---- InfluxDB ----//
  if (client.validateConnection()) {
    Serial.print("DB Connected: ");
    Serial.println(client.getServerUrl());
    stsDB = true;
  } else {
    Serial.print("DB not connected: ");
    Serial.println(client.getLastErrorMessage());
  }

  // Enable messages batching and retry buffer
  client.setWriteOptions(WriteOptions().writePrecision(WritePrecision::S).batchSize(10).bufferSize(20));

//  Serial.printf_P(PSTR("Before: free_heap %d, max_alloc_heap %d, heap_fragmentation  %d\n"), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation()); 

  Serial.print("\nChipId: ");
  Serial.println(sId);

  Serial.print("Chip ID: ");
  Serial.println(String(ESP.getFlashChipId(), HEX));

  Serial.print("Chip Real Size: ");
  Serial.println(ESP.getFlashChipRealSize());
  
  Serial.print("Chip Size: ");
  Serial.println(ESP.getFlashChipSize());
  
  Serial.print("Chip Speed: ");
  Serial.println(ESP.getFlashChipSpeed());
  
  Serial.print("Chip Mode: ");
  Serial.println(ESP.getFlashChipMode());

  Serial.println("\n**** EndSetup ****\n");
}


void updateTime(){
//---- Sincroniza hora desde NTP en un intervalo definido ----//
  if ((millis() - timeSyncLast >= NTP_UPDATE_INTERVAL)
    || timeSyncLast == 0) {
    timeSync(TZ_INFO, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
    timeSyncLast = millis();
  }
  return;
}

void updateLed(){
  if (!((millis() - statusLedLast >= STATUS_LED_INTERVAL)
    || statusLedLast == 0)) {
    return;
    }

  bool bLed = false;
  if (stsLed < 10) {
    if ((stsLed == 0) ||                               // Primer indicador siempre prende
        ((stsLed == 3) && drd->waitingForDRD())        // Segundo indicador es para el DRD
       ) {
      bLed = true;
    }
  }

  stsLed++;
  if (stsLed >= 40)
    stsLed = 0;


  if (bLed)
      digitalWrite(LED, LOW);   // Prendo LED
  else
      digitalWrite(LED, HIGH);   // Apago LED

  // digitalWrite(LED, LOW);       // Change the state of the LED
  // if (drd->waitingForDRD()) {
  //   delay(500);
  //   digitalWrite(LED, HIGH);      // Change the state of the LED
  // } else {
  //   delay(50);
  //   digitalWrite(LED, HIGH);      // Change the state of the LED
  // }
  statusLedLast = millis();
  return;
}


void loop() {
  //---- Servicios ----//
  ArduinoOTA.handle();
  ws.handleClient();                    // Listen for HTTP requests from clients
  if (drd->waitingForDRD())
    drd->loop();                        // Para la detección del DobleRst

  updateTime();
  updateLed();
  dbUpdateSensor();                     // Escribe un punto en la DB según el intervalo
}

