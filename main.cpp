#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Secrets.h> // Make sure this file exists and has your secrets
#include <esp_log.h>
#include <cstring>
#include <Config.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- Hardware Pins and Constants ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Deep Sleep Constants ---
#define BUTTON_MASK                 ( (1ULL << BUTTON_A_PIN) | (1ULL << BUTTON_B_PIN) | (1ULL << BUTTON_C_PIN) )

// --- Global Variables for Application State ---
String currentExpectedItemNames[MAX_EXPECTED_ITEMS];
String currentExpectedUIDStrings[MAX_EXPECTED_ITEMS];
int currentMaxItems = 0;
bool foundTagsDuringRepack[MAX_EXPECTED_ITEMS] = {false};
bool usedTagsInitially[MAX_EXPECTED_ITEMS] = {false}; // Tracks items that were "out" at session start

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;     // Change if you need a specific GMT offset for LOCAL display purposes
const int   daylightOffset_sec = 0; // Change for daylight saving for LOCAL display

String currentAssignedBagID = "";       // Airtable Record ID of the currently active bag
String currentAssignedBagName = "";     // Human-readable name of the active bag

// For listing bags in Admin Menu
#define MAX_BAGS_TO_LIST 10 // Max bags the ESP32 will try to list from Airtable
String availableBagNames[MAX_BAGS_TO_LIST];
String availableBagIDs[MAX_BAGS_TO_LIST];
int  availableBagCount = 0;

Adafruit_PN532 nfc(PN532_SDA, PN532_SCL);

enum SystemState {
  IDLE_MENU,
  REPACK_SESSION_START_CONFIRM,
  SESSION_ACTIVE,
  REPACKING_SCAN,
  REPACK_CONFIRM_FINISH,
  REPACK_SESSION_COMPLETE,
  ADMIN_MODE_UNLOCK,
  ADMIN_MODE_PREPARE_WIFI,
  ADMIN_MENU,
  ADMIN_SET_ACTIVE_BAG_FETCH, // New state to fetch bag list
  ADMIN_SET_ACTIVE_BAG_SELECT, // New state to select from list
  ADMIN_REPLACE_SCAN_OLD,
  ADMIN_REPLACE_SCAN_NEW,
  ADMIN_REPLACE_CONFIRM
};

SystemState currentState = IDLE_MENU;           // Default for cold boot
SystemState previousStateForRedraw = IDLE_MENU; // Not saved to RTC, re-eval on wake

enum MenuScreen {
  MAIN_MENU,
  ADMIN_MENU_SCREEN
};
MenuScreen currentMenuScreen = MAIN_MENU;       // Default for cold boot
int currentMenuSelection = 0;                   // Default for cold boot
bool redrawOled = true;                         // Flag to trigger OLED redraw

String admin_TargetOldUID_str = "";
String admin_NewUID_str = "";
String admin_NewEquipmentName_str = "";

// Button debouncing state
uint8_t buttonState[32];
uint8_t lastButtonState[32];
unsigned long lastDebounceTime[32];
const unsigned long debounceDelay = 50; // Debounce delay in ms
bool allRepackItemsScanned = false;     // True if all initially "used" items are found

// --- RTC Data Variables (persist through deep sleep) ---
RTC_DATA_ATTR SystemState savedCurrentState;
RTC_DATA_ATTR MenuScreen savedCurrentMenuScreen;
RTC_DATA_ATTR int savedCurrentMenuSelection;
RTC_DATA_ATTR bool rtcDataIsValid = false; // Flag to check if RTC data is valid from a previous save

// --- Inactivity Tracking ---
unsigned long lastActivityTime = 0; // Timestamp of the last user activity

//==============================================================================
// OLED HELPER FUNCTIONS
//==============================================================================
void oledClear() {
  display.clearDisplay();
}

void oledShow() {
  display.display();
}

void oledPrint(int x, int y, const String& text, int size = 1, bool wrap = true) {
  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.setTextWrap(wrap);
  display.print(text);
}

void oledDisplayMenu(const String& title, const String items[], int itemCount, int selection) {
  oledClear();
  oledPrint(0, 0, title, 1, false);
  display.drawFastHLine(0, 10, display.width(), SSD1306_WHITE); // Separator line

  int yPos = 16; // Starting Y position for menu items
  int lineHeight = 10;
  int maxVisibleItems = (SCREEN_HEIGHT - yPos) / lineHeight;
  int startItem = 0;

  // Logic for scrolling menu items if they exceed visible space
  if (itemCount > maxVisibleItems && selection >= maxVisibleItems - 1) {
    startItem = selection - (maxVisibleItems - 2);
    if (startItem < 0) {
      startItem = 0;
    }
  }

  for (int i = 0; i < itemCount; i++) {
    if (i >= startItem && i < startItem + maxVisibleItems) {
      String prefix = (selection == i) ? "> " : "  ";
      oledPrint(0, yPos + ((i - startItem) * lineHeight), prefix + items[i], 1, false);
    }
  }
  oledShow();
}

void oledShowStatusMessage(const String& line1, const String& line2 = "", const String& line3 = "", bool persistent = false, int customDuration = 0) {
  oledClear();
  oledPrint(0, 0, line1, 1, true);
  if (!line2.isEmpty()) {
    oledPrint(0, 18, line2, 1, true); // Adjusted Y for bicolor display
  }
  if (!line3.isEmpty()) {
    oledPrint(0, 28, line3, 1, true); // Adjusted Y for bicolor display
  }
  oledShow();

  if (!persistent) {
    delay(customDuration > 0 ? customDuration : STATUS_MESSAGE_DURATION_MS);
  }
}

void oledShowScanPrompt(const String& promptLine1, const String& promptLine2 = "") {
  oledClear();
  oledPrint(0, 0, promptLine1, 1, true);
  if (!promptLine2.isEmpty()) {
    oledPrint(0, 18, promptLine2, 1, true); // Adjusted Y for bicolor display
  }
  oledPrint(0, SCREEN_HEIGHT - 10, "B: Cancel/Back", 1, false); // Standard bottom prompt
  oledShow();
}

//==============================================================================
// BUTTON HANDLING
//==============================================================================
void setupButtons() {
  for (int i = 0; i < 32; i++) {
    buttonState[i] = LOW;
    lastButtonState[i] = LOW;
    lastDebounceTime[i] = 0;
  }
  pinMode(BUTTON_A_PIN, INPUT);
  pinMode(BUTTON_B_PIN, INPUT);
  pinMode(BUTTON_C_PIN, INPUT);
  Serial.println("Buttons Initialized (assuming external PULL-DOWN resistors).");
}

// Checks if a button is pressed and debounced. Updates lastActivityTime.
bool isButtonPressed(int pin) {
  if (pin < 0 || pin >= 32) {
    return false;
  }

  bool triggered = false;
  int reading = digitalRead(pin);

  if (reading != lastButtonState[pin]) {
    lastDebounceTime[pin] = millis();
  }

  if ((millis() - lastDebounceTime[pin]) > debounceDelay) {
    if (reading != buttonState[pin]) {
      buttonState[pin] = reading;
      if (buttonState[pin] == HIGH) { // Assumes buttons go HIGH when pressed
        triggered = true;
        Serial.printf("\nDEBUG: Button Pressed & Debounced (Pin %d went HIGH)\n", pin);
        lastActivityTime = millis(); // Reset inactivity timer on any confirmed button press
      }
    }
  }
  lastButtonState[pin] = reading;
  return triggered;
}

//==============================================================================
// UID AND STRING HELPERS
//==============================================================================
String uidBytesToHexString(const uint8_t* uid, uint8_t uidLength) {
  String hexString = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) {
      hexString += "0";
    }
    hexString += String(uid[i], HEX);
  }
  hexString.toUpperCase();
  return hexString;
}

bool compareUidStrings(const String& uidStr1, const String& uidStr2) {
  return uidStr1.equalsIgnoreCase(uidStr2);
}

String urlEncode(const String& str) {
  String encodedString = "";
  char c;
  char hexChars[17] = "0123456789ABCDEF"; // Lookup table for hex conversion

  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedString += c;
    } else {
      encodedString += '%';
      encodedString += hexChars[(c >> 4) & 0x0F];
      encodedString += hexChars[c & 0x0F];
    }
  }
  return encodedString;
}

//==============================================================================
// SPIFFS (FILE SYSTEM) OPERATIONS
//==============================================================================
bool saveListToSPIFFS() {
  Serial.println("Saving equipment list to SPIFFS...");
  File file = SPIFFS.open(EQUIPMENT_LIST_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open equipment list file for writing!");
    return false;
  }
  for (int i = 0; i < currentMaxItems; i++) {
    file.println(currentExpectedUIDStrings[i] + "," + currentExpectedItemNames[i]);
  }
  file.close();
  Serial.println("Equipment list saved to SPIFFS.");
  return true;
}

bool loadListFromSPIFFS() {
  if (!SPIFFS.begin(true)) { // Ensure SPIFFS is mounted, true formats if mount failed
    Serial.println("SPIFFS Mount Failed!");
    return false;
  }
  Serial.println("Loading equipment list from SPIFFS...");
  File file = SPIFFS.open(EQUIPMENT_LIST_FILE, FILE_READ);
  if (!file || file.isDirectory()) {
    Serial.println("Failed to open equipment list file for reading or file not found.");
    currentMaxItems = 0; // Ensure list is empty if file not found
    return false;
  }

  currentMaxItems = 0; // Reset before loading
  while (file.available() && currentMaxItems < MAX_EXPECTED_ITEMS) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      int commaIndex = line.indexOf(',');
      if (commaIndex > 0 && commaIndex < (int)line.length() - 1) {
        currentExpectedUIDStrings[currentMaxItems] = line.substring(0, commaIndex);
        currentExpectedItemNames[currentMaxItems] = line.substring(commaIndex + 1);
        currentMaxItems++;
      } else {
        Serial.println("Malformed line in equipment list file: " + line);
      }
    }
  }
  file.close();
  Serial.printf("Loaded %d items from SPIFFS.\n", currentMaxItems);
  return true;
}

//==============================================================================
// BAG CONFIGURATION (SPIFFS)
//==============================================================================
bool saveCurrentBagID(const String& bagID, const String& bagName) {
  Serial.printf("Saving current bag config to SPIFFS: ID=%s, Name=%s\n", bagID.c_str(), bagName.c_str());
  File file = SPIFFS.open(BAG_CONFIG_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open bag config file for writing!");
    return false;
  }
  file.println(bagID);    // Line 1: Bag Record ID
  file.println(bagName);  // Line 2: Bag Name (for display)
  file.close();
  currentAssignedBagID = bagID; // Update global variable
  currentAssignedBagName = bagName;
  Serial.println("Bag config saved to SPIFFS.");
  return true;
}

bool loadCurrentBagID() {
  if (!SPIFFS.exists(BAG_CONFIG_FILE)) {
    Serial.println("Bag config file not found. No active bag set.");
    currentAssignedBagID = "";
    currentAssignedBagName = "";
    return false;
  }

  File file = SPIFFS.open(BAG_CONFIG_FILE, FILE_READ);
  if (!file) {
    Serial.println("Failed to open bag config file for reading!");
    currentAssignedBagID = "";
    currentAssignedBagName = "";
    return false;
  }

  if (file.available()) {
    currentAssignedBagID = file.readStringUntil('\n');
    currentAssignedBagID.trim();
  } else {
    currentAssignedBagID = "";
  }
  
  if (file.available()) {
    currentAssignedBagName = file.readStringUntil('\n');
    currentAssignedBagName.trim();
  } else {
    currentAssignedBagName = ""; // If name wasn't saved or file is old format
  }
  
  file.close();

  if (!currentAssignedBagID.isEmpty()) {
    Serial.printf("Loaded active bag from SPIFFS: ID=%s, Name=%s\n", currentAssignedBagID.c_str(), currentAssignedBagName.c_str());
    return true;
  } else {
    Serial.println("No active bag ID found in config file.");
    currentAssignedBagName = ""; // Ensure name is also cleared if ID is missing
    return false;
  }
}

//==============================================================================
// TIME INITIALIZATION (NTP)
//==============================================================================
void initTime() {
  Serial.println("Configuring time from NTP server...");
  // configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); // Simpler version for one server
  
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){ // Attempt to get time, up to 10s default timeout
    Serial.println("Failed to obtain time from NTP server.");
    // You could add an OLED message here if desired
    // oledShowStatusMessage("Time Sync Fail", "NTP Error", "", false, 2000);
    return;
  }
  Serial.println("Time obtained successfully from NTP server:");
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S"); // Print formatted time to Serial
  // Optionally, show on OLED
  // char timeStr[30];
  // strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &timeinfo);
  // oledShowStatusMessage("Time Synced!", String(timeStr), "", false, 2000);
}

//==============================================================================
// WIFI OPERATIONS
//==============================================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  oledShowStatusMessage("Connecting WiFi", "Please wait...", "", true);
  Serial.print("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  String dots = ".";
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    oledShowStatusMessage("Connecting WiFi", dots, "", true); // Update progress on OLED
    dots += ".";
    if (dots.length() > 4) {
      dots = ".";
    }
    if (millis() - startTime > 20000) { // 20-second timeout
      Serial.println(" FAILED!");
      oledShowStatusMessage("WiFi FAILED!", "Check Network", "", false, 3000);
      return;
    }
  }
  Serial.println(" OK!");
  String ipAddr = WiFi.localIP().toString();
  Serial.println("WiFi Connected. IP Address: " + ipAddr);
  oledShowStatusMessage("WiFi Connected!", "", "", false, 1000);

  initTime(); // <--- CALL initTime() HERE AFTER SUCCESSFUL WIFI CONNECTION

  // Optionally, show a combined success message
  struct tm timeinfo;
  if(getLocalTime(&timeinfo, 1000)){ // Quick check if time is available (1s timeout)
      char dateStr[12];
      strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
      // oledShowStatusMessage("WiFi & Time OK!", "", String(dateStr), false, 2000);
  } else {
      oledShowStatusMessage("WiFi OK", "", "Time Sync Pend", false, 2000);
  }

}

void disconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(true); // true = also erase WiFi config from RAM for this session
    Serial.println("WiFi disconnected.");
    oledShowStatusMessage("WiFi Off", "", "", false, 1500);
  }
  WiFi.mode(WIFI_OFF); // Ensure WiFi module is powered down
  Serial.println("WiFi mode set to OFF.");
}

//==============================================================================
// AIRTABLE OPERATIONS (Replaces Google Sheets Operations)
//==============================================================================

// Helper to construct the Airtable API URL
String getAirtableApiUrl() {
  String tableNameEncoded = urlEncode(AIRTABLE_TABLE_NAME); // URL encode the table name
  return "https://api.airtable.com/v0/" + String(AIRTABLE_BASE_ID) + "/" + tableNameEncoded;
}

bool fetchEquipmentList_Airtable() {
  if (currentAssignedBagID.isEmpty()) {
    Serial.println("No active bag set. Cannot fetch equipment list.");
    oledShowStatusMessage("No Active Bag!", "Set in Admin Menu", "", false, 3000);
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      oledShowStatusMessage("Fetch Fail:", "No WiFi", "", false, 3000);
      return false;
    }
  }

  oledShowStatusMessage("Fetching List...", "For: " + currentAssignedBagName.substring(0,16), "From Airtable", "", true);
  Serial.println("Fetching equipment list from Airtable for bag ID: " + currentAssignedBagID);
  currentMaxItems = 0; // Clear current list

  String filterFormula = "filterByFormula=({Assigned Bag}='"; // <--- CHANGE "Assigned Bag" if your field name is different
  filterFormula += currentAssignedBagID;
  filterFormula += "')";
  
  String equipmentTableNameEncoded = urlEncode(AIRTABLE_TABLE_NAME); // AIRTABLE_TABLE_NAME should be "Equipment Pieces" or your equivalent
  String url = "https://api.airtable.com/v0/" + String(AIRTABLE_BASE_ID) + "/" + equipmentTableNameEncoded +
               "?" + filterFormula + 
               "&fields%5B%5D=UID&fields%5B%5D=Item%20Name"; // Assuming fields in "Equipment Pieces"

  Serial.println("Airtable Fetch URL: " + url);


  bool success = false;
  HTTPClient http;

  if (http.begin(url)) { // HTTPS by default if URL starts with https://
    http.addHeader("Authorization", "Bearer " + String(AIRTABLE_API_KEY));
    http.setTimeout(HTTP_TIMEOUT_MS);
    int httpCode = http.GET();
    Serial.printf("Airtable (Equipment) GET request, HTTP Code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      // Serial.println("Airtable Payload: " + payload); // For debugging
      
      // Adjust JsonDocument size based on expected payload size
      // For ~20 items, UID (14char) + Name (avg 20char) + JSON overhead
      // (14+20+~20overhead)*20 = 34*20*1.5 (safety) ~ 1KB. Add more for records, fields, id, createdTime.
      // Each record has roughly: {"id":"recXXX","createdTime":"XXX","fields":{"UID":"XXX","Item Name":"XXX"}} ~100-150 bytes
      // For MAX_EXPECTED_ITEMS = 20, try 20 * 150 bytes + overall structure = 3KB to 4KB
      DynamicJsonDocument doc(4096); // Increased size
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.printf("JSON Deserialization Failed: %s. Payload: %s\n", error.c_str(), payload.substring(0, 200).c_str());
        oledShowStatusMessage("Fetch Error:", "JSON Parse Fail", error.c_str(), false, 3000);
      } else {
        JsonArray records = doc["records"].as<JsonArray>();
        if (records.isNull()) {
            Serial.printf("Fetched JSON 'records' field is not an array or missing. Payload: %s\n", payload.substring(0, 200).c_str());
            oledShowStatusMessage("Fetch Error:", "No 'records' array", "", false, 3000);
        } else {
          int count = 0;
          for (JsonObject record : records) {
            if (count >= MAX_EXPECTED_ITEMS) {
              Serial.println("Max expected items reached, stopping parse.");
              break;
            }
            // --- IMPORTANT: Use the EXACT field names from your Airtable Base ---
            const char* uid_str = record["fields"]["UID"]; 
            const char* name_str = record["fields"]["Item Name"]; // If your primary field is "Name", use record["fields"]["Name"]

            if (uid_str && name_str) {
              currentExpectedUIDStrings[count] = String(uid_str);
              currentExpectedItemNames[count] = String(name_str);
              Serial.printf("Loaded: UID=%s, Name=%s\n", uid_str, name_str);
              count++;
            } else {
              Serial.println("Skipping item with missing UID or Item Name in JSON.");
              if (!uid_str) Serial.println("  UID field is missing or null.");
              if (!name_str) Serial.println("  Item Name field is missing or null.");
            }
          }
          currentMaxItems = count;
          Serial.printf("Loaded %d items from Airtable.\n", count);
          String itemsMessage = String(count) + " items found.";
          oledShowStatusMessage("Fetch OK!", itemsMessage, "", false, 2000);
          success = true; 
          saveListToSPIFFS(); 
        }
      }
    } else {
      Serial.printf("Airtable GET request failed, HTTP Code: %d\n", httpCode);
      String errorPayload = http.getString(); // Get error response
      Serial.println("Error payload: " + errorPayload);
      oledShowStatusMessage("Fetch Fail", "HTTP Err: " + String(httpCode), "", false, 3000);
    }
    http.end();
  } else {
    Serial.println("HTTPClient begin() failed for Airtable URL.");
    oledShowStatusMessage("Fetch Error:", "HTTP Begin Fail", "", false, 3000);
  }
  return success;
}


// To update a record in Airtable, we usually need its Airtable Record ID.
// So, first we fetch the Record ID using the targetUID (NFC UID).
String getAirtableRecordIdByUID(const String& nfcUID) {
  if (WiFi.status() != WL_CONNECTED) {
    // connectWiFi(); // Assuming WiFi is connected by calling function
    if (WiFi.status() != WL_CONNECTED) return "";
  }

  String recordId = "";
  // URL encode the NFC UID for the formula
  String filterFormula = "filterByFormula=({UID}='" + urlEncode(nfcUID) + "')"; 
  String url = getAirtableApiUrl() + "?" + filterFormula + "&fields%5B%5D=UID"; // Only need UID to confirm, Airtable sends ID anyway

  Serial.printf("Getting Record ID for UID: %s\n", nfcUID.c_str());
  
  HTTPClient http;
  if (http.begin(url)) {
    http.addHeader("Authorization", "Bearer " + String(AIRTABLE_API_KEY));
    http.setTimeout(HTTP_TIMEOUT_MS);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024); // Smaller doc for finding one record
      DeserializationError error = deserializeJson(doc, payload);
      if (!error && doc["records"] && doc["records"].is<JsonArray>() && doc["records"].size() > 0) {
        recordId = doc["records"][0]["id"].as<String>();
        Serial.println("Found Record ID: " + recordId);
      } else {
        Serial.println("Record not found by UID or JSON error.");
        if(error) Serial.println(error.c_str());
        Serial.println(payload);
      }
    } else {
      Serial.printf("Failed to get Record ID, HTTP: %d\n", httpCode);
      String errorPayload = http.getString();
      Serial.println("Error payload: " + errorPayload);
    }
    http.end();
  } else {
     Serial.println("HTTPClient begin() failed for getAirtableRecordIdByUID.");
  }
  return recordId;
}

bool sendAirtableUpdateRequest(const String& targetNFC_UID, const String& newNFC_UID, const String& newItemName) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      oledShowStatusMessage("Update Fail:", "No WiFi", "", false, 3000);
      return false;
    }
  }

  // Ensure time is somewhat valid if we are going to send a date
  struct tm timeinfo_check;
  if(!getLocalTime(&timeinfo_check, 1000) || timeinfo_check.tm_year < (2000-1900)){ // 1s timeout to get time
      Serial.println("NTP time not yet available for update. Trying to sync...");
      initTime(); // Try one more time to sync
      if(!getLocalTime(&timeinfo_check, 1000) || timeinfo_check.tm_year < (2000-1900)){
          oledShowStatusMessage("Update Warning:", "Accurate date", "unavailable.", false, 3000);
          // Decide if you want to proceed without a valid date or abort
          // For now, we'll proceed, and the date might be old/incorrect if NTP failed.
      }
  }

  String recordIdToUpdate = getAirtableRecordIdByUID(targetNFC_UID);
  if (recordIdToUpdate.isEmpty()) {
    Serial.println("Update failed: Could not find Airtable Record ID for target UID: " + targetNFC_UID);
    oledShowStatusMessage("Update Fail", "Old UID not found", targetNFC_UID.substring(0,8), false, 3000);
    return false;
  }

  String oledLine2 = targetNFC_UID.substring(0, 6) + "->" + newNFC_UID.substring(0, 6);
  String oledLine3 = newItemName.substring(0, 18);
  oledShowStatusMessage("Updating Airtable", oledLine2, oledLine3, true);
  Serial.printf("Updating Airtable Record ID %s: TargetOldUID:%s, NewUID:%s, NewName:%s\n", 
                recordIdToUpdate.c_str(), targetNFC_UID.c_str(), newNFC_UID.c_str(), newItemName.c_str());
  
  HTTPClient http;
  bool success = false;
  String url = getAirtableApiUrl(); // Base URL for PATCH operations on multiple records

  // Construct JSON payload for PATCH request
  // For updating specific records, the API expects an array of objects, each with "id" and "fields"
  DynamicJsonDocument updatePayload(512); // Sufficient for one record update
  JsonArray recordsArray = updatePayload.createNestedArray("records");
  JsonObject recordObject = recordsArray.createNestedObject();
  recordObject["id"] = recordIdToUpdate;
  JsonObject fieldsObject = recordObject.createNestedObject("fields");
  // --- IMPORTANT: Use the EXACT field names from your Airtable Base ---
  fieldsObject["UID"] = newNFC_UID;
  fieldsObject["Item Name"] = newItemName; // If your primary field is "Name", use "Name"

  time_t now;
  struct tm timeinfo;
  time(&now); // Gets current time (seconds since epoch, depends on system time being set)
  localtime_r(&now, &timeinfo);

  char isoTimestamp[25];
  // Format: YYYY-MM-DDTHH:MM:SSZ
  // strftime is powerful for this if time is set correctly (e.g., by NTP)
  if (timeinfo.tm_year > (2000 - 1900)) { // Basic check if time seems somewhat valid
      strftime(isoTimestamp, sizeof(isoTimestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
      fieldsObject["Last Scanned"] = isoTimestamp;
      Serial.println("Adding Last Scanned: " + String(isoTimestamp));
  } else {
      Serial.println("Time not set, cannot generate ISO timestamp for Last Scanned.");
      // Optionally, you could send a fixed past date or omit the field
      // fieldsObject["Last Scanned"] = "1970-01-01T00:00:00Z"; // Or handle as needed
  }

  String postData;
  serializeJson(updatePayload, postData);
  Serial.println("Airtable Update POST data: " + postData);
  
  if (http.begin(url)) {
    http.addHeader("Authorization", "Bearer " + String(AIRTABLE_API_KEY));
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

    // Airtable uses PATCH for updating records
    int httpCode = http.PATCH(postData); 
    Serial.printf("Airtable PATCH request, HTTP Code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String responsePayload = http.getString();
      Serial.printf("Airtable Response: %s\n", responsePayload.substring(0, min(300, (int)responsePayload.length())).c_str());
      // Check if the response contains the updated record, indicating success
      DynamicJsonDocument responseDoc(1024);
      deserializeJson(responseDoc, responsePayload);
      if (responseDoc["records"] && responseDoc["records"].size() > 0) {
         success = true;
         oledShowStatusMessage("Update Success!", "", "", false, 2000);
      } else {
         oledShowStatusMessage("Update OK?", "Resp. unclear", "", false, 3000);
         Serial.println("Update response OK, but record data not as expected in response.");
      }
    } else {
      String errorStr = http.getString();
      Serial.printf("Airtable PATCH request failed. Response: %s\n", errorStr.c_str());
      oledShowStatusMessage("Update Failed", "HTTP Err: " + String(httpCode), errorStr.substring(0,16), false, 4000);
    }
    http.end();
  } else {
    Serial.println("HTTPClient begin() failed for Airtable PATCH.");
    oledShowStatusMessage("Update Error", "HTTP Begin Fail", "", false, 3000);
  }
  return success;
}

bool fetchAvailableBags_Airtable() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi(); // Ensure WiFi is up, this also calls initTime()
    if (WiFi.status() != WL_CONNECTED) {
      oledShowStatusMessage("Bag Fetch Fail:", "No WiFi", "", false, 3000);
      return false;
    }
  }

  oledShowStatusMessage("Fetching Bags...", "From Airtable", "", true);
  Serial.println("Fetching available bags from Airtable 'Bags' table...");
  availableBagCount = 0; // Clear current list

  // Construct URL for the "Bags" table - IMPORTANT: Use the EXACT name of your "Bags" table.
  // If your Bags table is NOT named "Bags", change it here.
  String bagsTableNameEncoded = urlEncode("Bags"); // <--- CHANGE "Bags" IF YOUR TABLE HAS A DIFFERENT NAME
  String url = "https://api.airtable.com/v0/" + String(AIRTABLE_BASE_ID) + "/" + bagsTableNameEncoded +
               "?fields%5B%5D=Bag%20Name&view=Grid%20view"; // Assuming primary field is "Bag Name"
               // If your primary field in "Bags" table is different, change "Bag%20Name"

  bool success = false;
  HTTPClient http;

  if (http.begin(url)) {
    http.addHeader("Authorization", "Bearer " + String(AIRTABLE_API_KEY));
    http.setTimeout(HTTP_TIMEOUT_MS);
    int httpCode = http.GET();
    Serial.printf("Airtable (Bags) GET request, HTTP Code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(2048); // Adjust size if you have many bags or very long names
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.printf("Bags JSON Deserialization Failed: %s\n", error.c_str());
        oledShowStatusMessage("Bag Fetch Error:", "JSON Parse Fail", error.c_str(), false, 3000);
      } else {
        JsonArray records = doc["records"].as<JsonArray>();
        if (records.isNull()) {
            Serial.println("Fetched Bags JSON 'records' field is not an array or missing.");
            oledShowStatusMessage("Bag Fetch Error:", "No 'records' array", "", false, 3000);
        } else {
          for (JsonObject record : records) {
            if (availableBagCount >= MAX_BAGS_TO_LIST) {
              Serial.println("Max bags to list reached.");
              break;
            }
            const char* bagNameStr = record["fields"]["Bag Name"]; // <--- CHANGE "Bag Name" if your primary field has a different name
            const char* bagIdStr = record["id"]; // This is the Airtable Record ID

            if (bagNameStr && bagIdStr) {
              availableBagNames[availableBagCount] = String(bagNameStr);
              availableBagIDs[availableBagCount] = String(bagIdStr);
              Serial.printf("Found Bag: Name=%s, ID=%s\n", bagNameStr, bagIdStr);
              availableBagCount++;
            } else {
              Serial.println("Skipping bag with missing Name or ID in JSON.");
            }
          }
          Serial.printf("Loaded %d available bags from Airtable.\n", availableBagCount);
          if (availableBagCount > 0) {
            oledShowStatusMessage("Bag List OK!", String(availableBagCount) + " bags found.", "", false, 2000);
            success = true;
          } else {
            oledShowStatusMessage("No Bags Found", "Check Airtable", "'Bags' Table", false, 3000);
          }
        }
      }
    } else {
      Serial.printf("Airtable (Bags) GET request failed, HTTP Code: %d\n", httpCode);
      oledShowStatusMessage("Bag Fetch Fail", "HTTP Err: " + String(httpCode), "", false, 3000);
    }
    http.end();
  } else {
    Serial.println("HTTPClient begin() failed for Airtable (Bags) URL.");
    oledShowStatusMessage("Bag Fetch Err:", "HTTP Begin Fail", "", false, 3000);
  }
  return success;
}

//==============================================================================
// NFC TAG READING
//==============================================================================
bool readTagDetails(String& outUidString, String& outNdefName) {
  uint8_t uid[7]; // Max 7-byte UID for MIFARE tags
  uint8_t uidLength;
  bool nfcReadSuccess = false;
  outUidString = "";
  outNdefName = "";

  // Try to read a passive ISO14443A card
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) { // 50ms timeout
    outUidString = uidBytesToHexString(uid, uidLength);
    nfcReadSuccess = true; // At least UID was read

    // Attempt to read NDEF data, specifically for NTAG2xx series
    // NTAGs typically store NDEF data starting from page 4.
    // Each page is 4 bytes. Read 8 pages (32 bytes) to capture a short NDEF message.
    uint8_t pageBuffer[32]; 
    bool allPagesReadOk = true;
    for (int i = 0; i < 8; ++i) {
      if (!nfc.ntag2xx_ReadPage(4 + i, pageBuffer + (i * 4))) {
        allPagesReadOk = false;
        Serial.println("Failed to read NTAG page: " + String(4 + i));
        break;
      }
    }

    if (allPagesReadOk) {
      // Basic NDEF parsing: looking for a Text Record (Type 'T')
      // This is a simplified parser and might not cover all NDEF text record variations.
      int recordOffset = 0; // Start of NDEF data within pageBuffer

      // Check for NDEF Message TLV (0x03)
      if (pageBuffer[0] == 0x03) {
        uint8_t ndefMessageLength = pageBuffer[1];
        if (ndefMessageLength == 0xFF) { // Extended length format (not fully handled here)
          // recordOffset = 4; // Skip 0x03 FF LL1 LL2
          Serial.println("NDEF extended length format detected, parsing might be incomplete.");
          // For simplicity, we'll proceed assuming short length or first record starts after TLV
           recordOffset = 2; // Best guess for now
        } else {
          recordOffset = 2; // Skip 0x03 LEN
        }
      }
      // Else, assume NDEF record starts at pageBuffer[0] if no Message TLV

      // Now at 'recordOffset', expect the first NDEF Record Header
      // (MB ME SR TNF) TypeLen PayloadLen Type StatusByte Lang... Text...
      if (recordOffset < 28 && (pageBuffer[recordOffset] & 0x07) == 0x01) { // TNF = 0x01 (Well Known Type)
        uint8_t typeLength = pageBuffer[recordOffset + 1];
        uint8_t payloadLength = pageBuffer[recordOffset + 2]; // Note: can be 4 bytes if SR=0

        // Check if it's a Text Record ('T')
        if ((recordOffset + 3 + typeLength) < 32 && typeLength == 1 && pageBuffer[recordOffset + 3] == 'T') {
          int textPayloadFieldOffset = recordOffset + 3 + typeLength; // Start of Text Payload Field (Status Byte)
          uint8_t statusByte = pageBuffer[textPayloadFieldOffset];
          uint8_t langCodeLength = statusByte & 0x3F; // Bits 0-5 of Status Byte
          
          int actualTextStartOffset = textPayloadFieldOffset + 1 + langCodeLength;
          int actualTextLength = payloadLength - (1 + langCodeLength); // PayloadLen - StatusByte - LangCode

          if (actualTextStartOffset < 32 && actualTextLength > 0 && (actualTextStartOffset + actualTextLength) <= 32) {
            char nameBuffer[32]; // Temporary buffer for the text
            // Ensure copyLength doesn't exceed buffer or available data
            int copyLength = min(actualTextLength, (int)(sizeof(nameBuffer) - 1));
            copyLength = min(copyLength, (int)(32 - actualTextStartOffset));

            if (copyLength > 0) {
              memcpy(nameBuffer, &pageBuffer[actualTextStartOffset], copyLength);
              nameBuffer[copyLength] = '\0'; // Null-terminate the string
              outNdefName = String(nameBuffer);
              Serial.println("NDEF Text Record found: " + outNdefName);
            }
          } else {
            Serial.println("NDEF Text Record size/offset issue.");
          }
        } else {
          Serial.println("NDEF Record is not a Text Record or type length mismatch.");
        }
      } else {
        Serial.println("No Well-Known NDEF Record found at expected offset or buffer too small.");
      }
    } else {
      Serial.println("Failed to read sufficient NDEF pages for parsing.");
    }
  } // End of successful UID read
  
  return nfcReadSuccess; // True if UID was read, NDEF name is bonus
}


//==============================================================================
// REPACK SESSION LOGIC
//==============================================================================
void markAllItemsUsedInitially() {
  for (int i = 0; i < currentMaxItems; i++) {
    usedTagsInitially[i] = true;
    foundTagsDuringRepack[i] = false; // Reset found status for new repack
  }
  Serial.println("All items in list marked as 'Initially OUT' for repack session.");
  allRepackItemsScanned = false; // Reset this flag
}

void resetFoundTagsForRepack() {
  for (int i = 0; i < currentMaxItems; i++) {
    foundTagsDuringRepack[i] = false;
  }
  allRepackItemsScanned = false;
  Serial.println("Found tags reset for current repack scanning phase.");
}

int usedTagsInitiallyCount() {
  int count = 0;
  for (int i = 0; i < currentMaxItems; i++) {
    if (usedTagsInitially[i]) {
      count++;
    }
  }
  return count;
}

void processScannedRepackTag(const String& scannedUID) {
  bool matched = false;
  for (int i = 0; i < currentMaxItems; i++) {
    if (compareUidStrings(scannedUID, currentExpectedUIDStrings[i])) {
      matched = true;
      String itemName = currentExpectedItemNames[i];
      Serial.printf("Repack Scan: Matched '%s' (UID: %s)\n", itemName.c_str(), scannedUID.c_str());
      oledShowStatusMessage("Scanned:", itemName.substring(0, 18), scannedUID.substring(0, 8) + "...", false, 1500);
      
      if (!foundTagsDuringRepack[i]) {
        foundTagsDuringRepack[i] = true;
      } else {
        Serial.println("(Item already scanned in this repack session)");
        oledShowStatusMessage("Already Scanned!", itemName.substring(0, 18), "", false, 1000);
      }
      break; // Found the item, no need to check further
    }
  }

  if (!matched) {
    Serial.printf("Unknown Tag Scanned during Repack: %s\n", scannedUID.c_str());
    oledShowStatusMessage("Unknown Tag!", scannedUID.substring(0, 8) + "...", "", false, 1500);
  }

  // Check if all items that were initially marked as "used" are now "found"
  bool allDone = true;
  if (usedTagsInitiallyCount() == 0) { // If no items were meant to be repacked
      allDone = (currentMaxItems > 0); // Considered done if list isn't empty but nothing was "out"
                                       // or false if you want a specific "nothing to repack" state.
                                       // Let's say, if nothing was out, it's not "all packed" in the typical sense.
      allDone = false; 
  } else {
      for (int i = 0; i < currentMaxItems; i++) {
          if (usedTagsInitially[i] && !foundTagsDuringRepack[i]) {
              allDone = false; // At least one "used" item is still missing
              break;
          }
      }
  }
  allRepackItemsScanned = allDone;
}

void printCurrentBagStatusToSerial() {
  Serial.println("--- Current Bag Status (Serial Log) ---");
  if (currentMaxItems == 0) {
    Serial.println("(No equipment list loaded)");
    return;
  }

  int presentInBagCount = 0;
  int stillOutstandingCount = 0; 
  
  for (int i = 0; i < currentMaxItems; i++) {
    String itemStatusPrefix = "[AVAIL]"; // Default: available, not involved in current repack session
    if (usedTagsInitially[i]) { // Was this item part of the initial "out" set?
        if (foundTagsDuringRepack[i]) {
            itemStatusPrefix = "[IN]   "; // Was out, now scanned back in
            presentInBagCount++;
        } else {
            itemStatusPrefix = "[OUT]  "; // Was out, and still not scanned back
            stillOutstandingCount++;
        }
    } else if (foundTagsDuringRepack[i]) { // Scanned, but wasn't initially marked "out"
        itemStatusPrefix = "[UNEXP]"; // Unexpectedly found (e.g., added without being on "out" list)
        presentInBagCount++; // Still counts as present
    }
    Serial.println(itemStatusPrefix + " " + currentExpectedItemNames[i] + " (UID: " + currentExpectedUIDStrings[i] + ")");
  }

  Serial.printf("Summary: Scanned In: %d, Initially Used: %d, Still Outstanding: %d, Total List: %d\n",
    presentInBagCount, usedTagsInitiallyCount(), stillOutstandingCount, currentMaxItems);
  Serial.println("---------------------------------------");
}

void displayBagStatusSummaryOLED() {
  if (currentMaxItems == 0) {
    oledShowStatusMessage("Bag Status:", "No List!", "", true);
    return;
  }

  int itemsScannedThisRepack = 0;
  int itemsStillMissingFromInitial = 0;
  int initialItemsToFind = usedTagsInitiallyCount();

  for (int i = 0; i < currentMaxItems; i++) {
    if (foundTagsDuringRepack[i]) { 
      itemsScannedThisRepack++;
    }
    if (usedTagsInitially[i] && !foundTagsDuringRepack[i]) {
      itemsStillMissingFromInitial++;
    }
  }
  
  String line1 = "Repack: " + String(itemsScannedThisRepack) + "/" + String(initialItemsToFind);
  String line2 = "Missing: " + String(itemsStillMissingFromInitial);
  String line3 = "Total List: " + String(currentMaxItems);
  oledShowStatusMessage(line1, line2, line3, true); // Persistent display
}

void reportSessionOutcomeToSerial() {
  Serial.println("--- Repack Session Outcome ---");
  if (currentMaxItems == 0) {
    Serial.println("(No equipment list loaded for this session)");
    return;
  }
  
  int initialOutCount = usedTagsInitiallyCount();
  if (initialOutCount == 0) {
    Serial.println("No items were marked as 'used' at the start of this repack session.");
    Serial.println("----------------------------");
    return;
  }

  int missingItemCount = 0;
  bool anyItemsMissing = false;
  Serial.println("Items NOT scanned back (that were initially 'OUT'):");
  for (int i = 0; i < currentMaxItems; i++) {
    if (usedTagsInitially[i] && !foundTagsDuringRepack[i]) {
      Serial.println("- " + currentExpectedItemNames[i] + " (UID: " + currentExpectedUIDStrings[i] + ")");
      missingItemCount++;
      anyItemsMissing = true;
    }
  }

  if (!anyItemsMissing) {
    Serial.println("All items initially marked as 'OUT' were successfully scanned back!");
  } else {
    Serial.printf("%d item(s) initially marked as 'OUT' are still recorded as missing.\n", missingItemCount);
  }
  Serial.println("----------------------------");
}

void displaySessionOutcomeOLED() {
  int missingItemCount = 0;
  bool anyMissing = false;
  int initialItemsOut = usedTagsInitiallyCount();

  if (currentMaxItems > 0 && initialItemsOut > 0) {
    for (int i = 0; i < currentMaxItems; i++) {
      if (usedTagsInitially[i] && !foundTagsDuringRepack[i]) {
        anyMissing = true;
        missingItemCount++;
      }
    }
  }

  if (!anyMissing && currentMaxItems > 0 && initialItemsOut > 0) {
    oledShowStatusMessage("🎉 WELL DONE! 🎉", "All items packed!", "", true);
  } else if (currentMaxItems == 0 || initialItemsOut == 0) {
    oledShowStatusMessage("Session Done", "(No items out", "or list empty)", true);
  } else { // Items were out, and some are still missing
    String line2 = String(missingItemCount) + " item(s) still";
    oledShowStatusMessage("Session Done", line2, "marked as OUT.", true);
  }
}


//==============================================================================
// MENU DISPLAY & NAVIGATION
//==============================================================================
void displayCurrentMenuOnOLED() {
  oledClear(); // Clear display at the beginning of any menu draw

  if (currentMenuScreen == MAIN_MENU) {
    String items[] = {"Start Repack", "Admin Mode"};
    // Assuming oledDisplayMenu is a generic function you still want to use for simple menus
    // It will handle its own title, items, count, and selection display
    oledDisplayMenu("MAIN MENU", items, 2, currentMenuSelection); 
    // oledDisplayMenu should end with oledShow() and set redrawOled = false;
    // If oledDisplayMenu doesn't set redrawOled, uncomment the line below
    // redrawOled = false; 

  } else if (currentMenuScreen == ADMIN_MENU_SCREEN) {
    // --- Custom Drawing Logic for Admin Menu ---
    String adminItems[] = {"Set Active Bag", "Replace Tag", "Fetch List", "Exit Admin"};
    int adminItemCount = 4; // This must be accurate

    String adminTitleLine1 = "ADMIN (WiFi " + String(WiFi.status() == WL_CONNECTED ? "ON" : "OFF") + ")";
    String adminTitleLine2 = "";
    if (!currentAssignedBagName.isEmpty()) {
        adminTitleLine2 = "Bag: " + currentAssignedBagName.substring(0,18); 
    } else {
        adminTitleLine2 = "Bag: (None Set)";
    }

    // --- Title Y-Positions ---
    int currentY = 0;
    oledPrint(0, currentY, adminTitleLine1, 1, false); 
    currentY += 8; 

    oledPrint(0, currentY, adminTitleLine2, 1, false); 
    currentY += 8; 

    int separatorY = currentY + 2; 
    display.drawFastHLine(0, separatorY, display.width(), SSD1306_WHITE);
    
    currentY = separatorY + 4; // Starting Y for the menu items list 

    // --- Simplified Item Drawing (No Scrolling, No Arrows) ---
    int lineHeight = 10; 
    for (int i = 0; i < adminItemCount; i++) { // Iterate through all admin items
        String prefix = (currentMenuSelection == i) ? "> " : "  ";
        int itemY = currentY + (i * lineHeight);
        // No need to check if itemY is off-screen if we are sure they fit
        oledPrint(0, itemY, prefix + adminItems[i], 1, false);
    }

    oledShow(); // Update the display with all drawn elements
    redrawOled = false;
  }
}

// Handles UP/DOWN button presses for menu navigation. SELECT is handled by state.
void handleButtonInputsForMenu() {
  int itemCount = 0;
  if (currentMenuScreen == MAIN_MENU) {
    itemCount = 2;
  } else if (currentMenuScreen == ADMIN_MENU_SCREEN) {
    itemCount = 4;
  } else {
    return; // Not a known menu screen
  }

  if (isButtonPressed(BUTTON_A_PIN)) { // UP
    currentMenuSelection = (currentMenuSelection - 1 + itemCount) % itemCount;
    redrawOled = true;
    Serial.printf("Menu Navigation: UP, New Selection: %d\n", currentMenuSelection);
  } else if (isButtonPressed(BUTTON_B_PIN)) { // DOWN
    currentMenuSelection = (currentMenuSelection + 1) % itemCount;
    redrawOled = true;
    Serial.printf("Menu Navigation: DOWN, New Selection: %d\n", currentMenuSelection);
  }
}

//==============================================================================
// STATE HANDLERS
//==============================================================================
void handleIdleMenuState() {
  handleButtonInputsForMenu(); 

  if (redrawOled) {
    displayCurrentMenuOnOLED();
  }

  if (isButtonPressed(BUTTON_C_PIN)) { // SELECT action for the current menu
    redrawOled = true; 
    if (currentMenuScreen == MAIN_MENU) {
      if (currentMenuSelection == 0) { // "Start Repack" selected
        currentState = REPACK_SESSION_START_CONFIRM;
      } else if (currentMenuSelection == 1) { // "Admin Mode" selected
        currentState = ADMIN_MODE_UNLOCK;
      }
    }
  }

  // Check for inactivity timeout to initiate deep sleep
  if ((millis() - lastActivityTime) > DEEP_SLEEP_TIMEOUT_MS) {
    Serial.println("IDLE_MENU: Inactivity timeout. Preparing for deep sleep.");
    
    // Save current state to RTC memory
    savedCurrentState = currentState;         // Should be IDLE_MENU
    savedCurrentMenuScreen = currentMenuScreen; // Should be MAIN_MENU if in this state handler
    savedCurrentMenuSelection = currentMenuSelection;
    rtcDataIsValid = true; // Mark RTC data as valid for next wake-up

    oledClear();
    oledPrint(0, 0, "Sleeping...");
    oledShow();
    delay(1000); // Brief display of "Sleeping..."
    display.ssd1306_command(SSD1306_DISPLAYOFF); // Turn off OLED panel to save power

    // Configure ESP32 to wake up on any button press (HIGH signal)
    esp_sleep_enable_ext1_wakeup(BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.println("Configured ext1 wakeup for buttons. Entering deep sleep now.");
    esp_deep_sleep_start();
  }
}


void handleRepackSessionStartConfirmState() {
  static bool oledPromptDrawn = false; // Track if the prompt for this state is on OLED

  if (!oledPromptDrawn || redrawOled) {
    if (currentAssignedBagID.isEmpty()) {
      Serial.println("Repack Confirm: No equipment list loaded. C: Back to Menu.");
      oledShowStatusMessage("No Active Bag!", "Admin->Fetch", "C: Menu", true);
    } else if (currentMaxItems == 0) { // Check if the list for the active bag is loaded/empty
      Serial.println("Repack Confirm: Equipment list for " + currentAssignedBagName + " is empty. C: Back to Menu.");
      oledShowStatusMessage("List Empty For:", currentAssignedBagName.substring(0,18), "Fetch in Admin. C:Menu", true);
    } else {
      Serial.println("Repack Confirm: Start session for " + currentAssignedBagName + "? A=Yes, B=No/Back.");
      String line1 = "Start Repack for:";
      String line2 = currentAssignedBagName.substring(0,18); // Show current bag name
      String line3 = String(currentMaxItems) + " items. A:Yes B:No"; // Removed "/Back" as B is just No
      oledShowStatusMessage(line1, line2, line3, true);
    }
    oledPromptDrawn = true;
    redrawOled = false;
  }

  if (currentMaxItems == 0) { // Special case if no list is loaded
    if (isButtonPressed(BUTTON_C_PIN)) { // Only C (Back to Menu) is active
      currentState = IDLE_MENU;
      currentMenuScreen = MAIN_MENU; 
      currentMenuSelection = 0; 
      oledPromptDrawn = false;      // Reset flag for next entry
      redrawOled = true;
    }
    return; // No other buttons active if list is empty
  }

  if (isButtonPressed(BUTTON_A_PIN)) { // Yes, start repack session
    markAllItemsUsedInitially(); 
    currentState = SESSION_ACTIVE; 
    printCurrentBagStatusToSerial(); // Log initial status after marking items
    oledPromptDrawn = false;
    redrawOled = true;
  } else if (isButtonPressed(BUTTON_B_PIN)) { // No, or back to main menu
    currentState = IDLE_MENU;
    currentMenuScreen = MAIN_MENU;
    currentMenuSelection = 0; 
    oledPromptDrawn = false;
    redrawOled = true;
  }
}

void handleSessionActiveState() {
  static bool oledPromptDrawn = false;

  if (!oledPromptDrawn || redrawOled) {
    String itemsOutMsg = String(usedTagsInitiallyCount()) + " items OUT";
    Serial.println("Session Active. " + itemsOutMsg + ". C: Start Scan.");
    oledShowStatusMessage("Session Active", itemsOutMsg, "C: Start Scan", true);
    oledPromptDrawn = true;
    redrawOled = false;
  }

  if (isButtonPressed(BUTTON_C_PIN)) { // Start Scanning
    resetFoundTagsForRepack();    // Prepare for new scan phase
    currentState = REPACKING_SCAN;
    displayBagStatusSummaryOLED(); // Update OLED for the scanning phase
    oledPromptDrawn = false;
    redrawOled = true;
  }
  // TODO: Consider adding a Button B option to cancel the active session and return to IDLE_MENU.
}

void handleRepackingScanState() {
  static unsigned long lastNfcPollTime = 0;
  static bool oledScreenDrawn = false; // Tracks if combined status/prompt is drawn

  if (!oledScreenDrawn || redrawOled) {
    Serial.println("REPACKING State: Scan items. B: Manual Finish.");
    displayBagStatusSummaryOLED(); // Show current packing status (persistent part)
    // Add the specific prompt for this state below the status
    oledPrint(0, SCREEN_HEIGHT - 10, "B: Manual Finish", 1, false); 
    oledShow(); // Refresh the display with both status and prompt
    oledScreenDrawn = true;
    redrawOled = false;
  }

  if ((millis() - lastNfcPollTime) >= NFC_POLLING_INTERVAL_MS) {
    lastNfcPollTime = millis();
    String uidScanned, nameScanned;
    if (readTagDetails(uidScanned, nameScanned)) {
      processScannedRepackTag(uidScanned); // Process the scanned tag
      
      // After processing, refresh the OLED to show updated status and re-draw prompt
      displayBagStatusSummaryOLED();
      oledPrint(0, SCREEN_HEIGHT - 10, "B: Manual Finish", 1, false);
      oledShow();
      
      delay(TAG_READ_DELAY_MS); // Brief pause to prevent immediate re-scan of same tag
      lastNfcPollTime = millis(); // Reset poll timer after handling a tag

      // Check if all required items are packed
      if (allRepackItemsScanned && usedTagsInitiallyCount() > 0) {
        Serial.println("All initially 'OUT' items have been scanned back!");
        oledShowStatusMessage("🎉 All Packed! 🎉", "All items found!", "", false, 2000);
        currentState = REPACK_SESSION_COMPLETE;
        oledScreenDrawn = false; // Reset for next state
        redrawOled = true;
        return; // Exit state handler early
      }
    }
  }

  if (isButtonPressed(BUTTON_B_PIN)) { // User opts to finish manually
    currentState = REPACK_CONFIRM_FINISH;
    oledScreenDrawn = false;
    redrawOled = true;
  }
}

void handleRepackConfirmFinishState() {
  static bool oledPromptDrawn = false;

  if (!oledPromptDrawn || redrawOled) {
    Serial.println("Confirm Finish Manually? A=YES, B=Back to Scan.");
    oledShowStatusMessage("Finish Manually?", "Some items may be", "still OUT.", true);
    // Add specific button prompts for this confirmation screen
    oledPrint(0, SCREEN_HEIGHT - 20, "A:Yes B:No/Scan", 1, false); 
    oledShow();
    oledPromptDrawn = true;
    redrawOled = false;
  }

  if (isButtonPressed(BUTTON_A_PIN)) { // Yes, confirm manual finish
    currentState = REPACK_SESSION_COMPLETE;
    oledPromptDrawn = false;
    redrawOled = true;
  } else if (isButtonPressed(BUTTON_B_PIN)) { // No, go back to scanning
    currentState = REPACKING_SCAN;
    Serial.println("Returning to scanning phase.");
    oledPromptDrawn = false;
    redrawOled = true;
  }
}

void handleRepackSessionCompleteState() {
  static bool oledOutcomeDrawn = false; // Tracks if outcome message is on OLED
  static unsigned long entryTime = 0;   // Timestamp for auto-return timeout

  if (!oledOutcomeDrawn || redrawOled) {
    reportSessionOutcomeToSerial(); // Log detailed outcome to Serial
    displaySessionOutcomeOLED();    // Show summary outcome on OLED (this is persistent)
    
    Serial.println("Repack Session Complete. C: Main Menu (or auto-return).");
    // Add "C: Main Menu" prompt to the persistent OLED display
    oledPrint(0, SCREEN_HEIGHT - 10, "C: Main Menu", 1, false);
    oledShow(); // Refresh OLED with the added prompt

    oledOutcomeDrawn = true;
    redrawOled = false;
    entryTime = millis(); // Start timeout for auto-returning to main menu
  }

  // Check for user action or timeout
  if (isButtonPressed(BUTTON_C_PIN) || 
      ((millis() - entryTime) > WELL_DONE_TIMEOUT_MS && entryTime != 0)) {
    currentState = IDLE_MENU;
    currentMenuScreen = MAIN_MENU;
    currentMenuSelection = 0;
    oledOutcomeDrawn = false; // Reset flag for next entry
    redrawOled = true;
    entryTime = 0;           // Reset timer
  }
}

void handleAdminModeUnlockState() {
  static bool oledPromptDrawn = false;
  static unsigned long unlockAttemptStartTime = 0; // Timeout for this specific attempt

  if (!oledPromptDrawn || redrawOled) {
    Serial.println("ADMIN UNLOCK: Scan Admin Tag. B: Back to Main Menu.");
    oledShowScanPrompt("ADMIN UNLOCK:", "Scan Admin Tag"); // Shows generic B:Cancel/Back
    oledPromptDrawn = true;
    redrawOled = false;
    unlockAttemptStartTime = millis(); 
  }

  String scannedUID, scannedName;
  if (readTagDetails(scannedUID, scannedName)) { // Attempt to read a tag
    if (!scannedUID.isEmpty() && scannedUID.equalsIgnoreCase(ADMIN_TAG_UID_STRING)) {
      Serial.println("Admin Tag Scanned and Verified!");
      oledShowStatusMessage("Admin Tag OK!", "", "", false, 1500);
      currentState = ADMIN_MODE_PREPARE_WIFI;
      oledPromptDrawn = false;
      redrawOled = true;
      return; // Successfully unlocked
    } else if (!scannedUID.isEmpty()) { // A tag was scanned, but it's not the admin tag
      Serial.println("Wrong Tag Scanned for Admin Unlock: " + scannedUID);
      oledShowStatusMessage("Wrong Tag!", scannedUID.substring(0, 8) + "...", "Scan Admin Tag", false, 2000);
      delay(TAG_READ_DELAY_MS); 
      // Force redraw of the prompt for another attempt
      oledPromptDrawn = false; 
      redrawOled = true; 
      unlockAttemptStartTime = millis(); // Reset timeout for the new attempt
    }
    // If readTagDetails returns false, no tag was presented, just continue polling/timeout
  }

  if (isButtonPressed(BUTTON_B_PIN)) { // User cancels unlock attempt
    Serial.println("Admin Unlock cancelled by user. Returning to Main Menu.");
    oledShowStatusMessage("Admin Cancelled", "", "", false, 1500);
    currentState = IDLE_MENU;
    currentMenuScreen = MAIN_MENU;
    currentMenuSelection = 0;
    oledPromptDrawn = false;
    redrawOled = true;
  }

  // Check for timeout waiting for admin tag
  if ((millis() - unlockAttemptStartTime) > ADMIN_TAG_SCAN_TIMEOUT_MS) {
    Serial.println("Timeout waiting for Admin Tag scan.");
    oledShowStatusMessage("Timeout!", "No Admin Tag", "", false, 2000);
    currentState = IDLE_MENU;
    currentMenuScreen = MAIN_MENU;
    currentMenuSelection = 0;
    oledPromptDrawn = false;
    redrawOled = true;
  }
}

void handleAdminModePrepareWifiState() {
  // This is a transient state, primarily for initiating WiFi connection.
  Serial.println("Admin Mode: Preparing WiFi connection...");
  oledShowStatusMessage("Admin Mode", "Connecting WiFi...", "", true); // Show persistent message

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi(); // This function handles its own OLED status messages during connection
  } else {
    Serial.println("WiFi already connected.");
    initTime();
    oledShowStatusMessage("Admin Mode", "WiFi Ready", "", false, 1500); // Brief confirmation
  }

  if (WiFi.status() == WL_CONNECTED) {
    struct tm timeinfo_check;
    if(!getLocalTime(&timeinfo_check, 1000) || timeinfo_check.tm_year < (2000-1900)){
        Serial.println("Warning: Admin mode entered, NTP time might not be fully synced yet.");
        // No need for an OLED warning here unless it blocks critical functionality.
    }

    Serial.println("WiFi connection successful for Admin Mode.");
    currentMenuScreen = ADMIN_MENU_SCREEN;
    currentMenuSelection = 0; // Default to first admin menu option
    currentState = ADMIN_MENU;
  } else {
    Serial.println("WiFi connection FAILED. Admin Mode cannot proceed. Returning to Main Menu.");
    // connectWiFi() would have shown a failure message on OLED
    currentState = IDLE_MENU;
    currentMenuScreen = MAIN_MENU;
    currentMenuSelection = 0;
  }
  redrawOled = true; // Ensure the next state's screen is drawn
}

void handleAdminMenuState() {
  if (redrawOled) {
    displayCurrentMenuOnOLED(); // Displays admin menu with WiFi status
  }
  handleButtonInputsForMenu(); // Handles UP/DOWN navigation

  if (isButtonPressed(BUTTON_C_PIN)) { // SELECT action
    redrawOled = true; 
    // currentMenuSelection is now 0-3
    if (currentMenuSelection == 0) {        // "Set Active Bag"
      currentState = ADMIN_SET_ACTIVE_BAG_FETCH;
    } else if (currentMenuSelection == 1) { // "Replace Tag"
      if (currentAssignedBagID.isEmpty()) {
        oledShowStatusMessage("Action Failed", "No Active Bag Set", "Use 'Set Bag'", false, 3000);
        // currentState remains ADMIN_MENU
        redrawOled = true; // Force redraw of admin menu
      } else {
        currentState = ADMIN_REPLACE_SCAN_OLD;
        admin_TargetOldUID_str = ""; 
        admin_NewUID_str = "";
        admin_NewEquipmentName_str = "";
      }
    } else if (currentMenuSelection == 2) { // "Fetch List (Active Bag)"
      Serial.println("Admin Menu: User selected 'Fetch List (Active Bag)'.");
      if (currentAssignedBagID.isEmpty()) {
        oledShowStatusMessage("Fetch Failed", "No Active Bag Set", "Use 'Set Bag'", false, 3000);
        // currentState remains ADMIN_MENU
        redrawOled = true; // Force redraw of admin menu
      } else {
        if (fetchEquipmentList_Airtable()) { 
          Serial.println("Equipment list fetched successfully from Admin Menu for active bag.");
        } else {
          Serial.println("Failed to fetch equipment list from Admin Menu for active bag.");
        }
        // Stay in admin menu, redraw it (in case WiFi status changed or to clear fetch messages)
        // currentState remains ADMIN_MENU
        redrawOled = true; 
      }
    } else if (currentMenuSelection == 3) { // "Exit Admin"
      Serial.println("Exiting Admin Mode...");
      oledShowStatusMessage("Exiting Admin...", "", "", false, 1000);
      disconnectWiFi(); 
      currentMenuScreen = MAIN_MENU;
      currentMenuSelection = 0;
      currentState = IDLE_MENU;
    }
  }
  // Note: No deep sleep from Admin Menu in this version to simplify WiFi state management.
  // If desired, ensure WiFi is disconnected before sleeping from this state.
}

void handleAdminReplaceScanOldState() {
  static bool oledPromptDrawn = false;

  if (redrawOled || !oledPromptDrawn) {
    Serial.println("ADMIN REPLACE: Scan OLD Tag to be replaced. B: Cancel.");
    oledShowScanPrompt("Scan OLD Tag", "(Tag to replace)");
    oledPromptDrawn = true;
    redrawOled = false;
  }

  String uidScanned, nameScanned;
  if (readTagDetails(uidScanned, nameScanned)) { // Attempt to read tag
    if (!uidScanned.isEmpty()) {
      admin_TargetOldUID_str = uidScanned;
      Serial.printf("ADMIN: OLD Tag Scanned: UID=%s, Name='%s'\n", uidScanned.c_str(), nameScanned.c_str());
      oledShowStatusMessage("OLD Tag OK:", uidScanned.substring(0, 8) + "...", nameScanned.substring(0, 18), false, 1500);
      currentState = ADMIN_REPLACE_SCAN_NEW;
      oledPromptDrawn = false;
      redrawOled = true;
      delay(TAG_READ_DELAY_MS); // Brief pause before next screen
    }
  }

  if (isButtonPressed(BUTTON_B_PIN)) { // Cancel
    Serial.println("Admin Replace Tag (Scan OLD) cancelled. Returning to Admin Menu.");
    oledShowStatusMessage("Cancelled", "Admin Menu", "", false, 1500);
    currentState = ADMIN_MENU;
    oledPromptDrawn = false;
    redrawOled = true;
  }
}

void handleAdminReplaceScanNewState() {
  static bool oledPromptDrawn = false;

  if (redrawOled || !oledPromptDrawn) {
    Serial.println("ADMIN REPLACE: Scan NEW replacement Tag. B: Cancel.");
    oledShowScanPrompt("Scan NEW Tag", "(Replacement Tag)");
    oledPromptDrawn = true;
    redrawOled = false;
  }

  String uidScanned, nameScanned;
  if (readTagDetails(uidScanned, nameScanned)) {
    if (!uidScanned.isEmpty()) {
      admin_NewUID_str = uidScanned;
      admin_NewEquipmentName_str = nameScanned; // Capture name from NDEF
      Serial.printf("ADMIN: NEW Tag Scanned: UID=%s, Name='%s'\n", uidScanned.c_str(), nameScanned.c_str());
      
      // Validate NEW tag
      if (admin_NewUID_str.equalsIgnoreCase(admin_TargetOldUID_str)) {
        Serial.println("Error: NEW Tag UID is identical to OLD Tag UID.");
        oledShowStatusMessage("Error: Same UID!", "Scan different NEW", "B: Cancel", false, 3000);
        delay(TAG_READ_DELAY_MS); // Allow message to be seen
        // Force redraw of current prompt to re-scan
        oledPromptDrawn = false; 
        redrawOled = true; 
        return; // Stay in this state to re-scan
      }
      
      if (admin_NewEquipmentName_str.isEmpty()) {
        Serial.println("Error: NEW Tag has no NDEF Name. Tag must be programmed with a name.");
        oledShowStatusMessage("Error: No Name!", "Program NEW Tag", "B: Cancel", false, 3000);
        delay(TAG_READ_DELAY_MS);
        oledPromptDrawn = false;
        redrawOled = true;
        return; // Stay in this state
      }

      // If validations pass
      oledShowStatusMessage("NEW Tag OK:", uidScanned.substring(0, 8) + "...", nameScanned.substring(0, 18), false, 1500);
      currentState = ADMIN_REPLACE_CONFIRM;
      oledPromptDrawn = false;
      redrawOled = true;
      delay(TAG_READ_DELAY_MS); 
    }
  }

  if (isButtonPressed(BUTTON_B_PIN)) { // Cancel
    Serial.println("Admin Replace Tag (Scan NEW) cancelled. Returning to Admin Menu.");
    oledShowStatusMessage("Cancelled", "Admin Menu", "", false, 1500);
    currentState = ADMIN_MENU;
    oledPromptDrawn = false;
    redrawOled = true;
  }
}

void handleAdminReplaceConfirmState() {
  static bool oledPromptDrawn = false;

  if (redrawOled || !oledPromptDrawn) {
    Serial.println("ADMIN REPLACE: Confirm Replacement Details");
    Serial.println("  OLD UID: " + admin_TargetOldUID_str);
    Serial.println("  NEW UID: " + admin_NewUID_str + ", New Name: '" + admin_NewEquipmentName_str + "'");
    Serial.println("Press A to Confirm, B to Cancel.");

    oledClear();
    oledPrint(0, 0, "Confirm Replace?");
    oledPrint(0, 10, "OLD:" + admin_TargetOldUID_str.substring(0, 16)); 
    oledPrint(0, 20, "NEW:" + admin_NewUID_str.substring(0, 16));
    oledPrint(0, 30, admin_NewEquipmentName_str.substring(0, 21)); 
    oledPrint(0, SCREEN_HEIGHT - 18, "A:Confirm B:Cancel", 1, false);
    oledShow();
    
    oledPromptDrawn = true;
    redrawOled = false;
  }

  if (isButtonPressed(BUTTON_A_PIN)) { // Confirm replacement
    Serial.println("CONFIRMED. Sending update to Google Sheet...");
    // sendSheetUpdateRequest() will display its own success/failure messages
    if (sendAirtableUpdateRequest(admin_TargetOldUID_str, admin_NewUID_str, admin_NewEquipmentName_str)) {
      Serial.println("Airtable update reported success. Attempting to re-fetch local list...");
      // fetchEquipmentList() will display its own messages
      if (fetchEquipmentList_Airtable()) {
        Serial.println("Local equipment list refreshed successfully after update.");
      } else {
        Serial.println("Error re-fetching list after update. Advise manual fetch.");
        oledShowStatusMessage("Airtable Updated", "List fetch FAILED", "Fetch manually", false, 3000);
      }
    } else {
      Serial.println("Error reported during Airtable update.");
      // Error message already shown by sendSheetUpdateRequest
    }
    currentState = ADMIN_MENU; // Return to Admin Menu regardless of update outcome
    oledPromptDrawn = false;
    redrawOled = true;
  } else if (isButtonPressed(BUTTON_B_PIN)) { // Cancel confirmation
    Serial.println("Admin Replace Confirm cancelled by user. Returning to Admin Menu.");
    oledShowStatusMessage("Cancelled", "Admin Menu", "", false, 1500);
    currentState = ADMIN_MENU;
    oledPromptDrawn = false;
    redrawOled = true;
  }
}

void handleAdminSetActiveBagFetchState() {
  Serial.println("ADMIN_SET_ACTIVE_BAG_FETCH: Attempting to fetch list of bags.");
  // fetchAvailableBags_Airtable() handles its own OLED messages
  if (fetchAvailableBags_Airtable()) {
    if (availableBagCount > 0) {
      currentState = ADMIN_SET_ACTIVE_BAG_SELECT;
      currentMenuSelection = 0; // Reset selection for the new list
    } else {
      // No bags found or error, message already shown by fetchAvailableBags_Airtable
      oledShowStatusMessage("No Bags Found", "Check Airtable", "Admin Menu", false, 3000);
      currentState = ADMIN_MENU; // Go back to admin menu
    }
  } else {
    // Error fetching bags, message already shown
    currentState = ADMIN_MENU; // Go back to admin menu
  }
  redrawOled = true;
}

void handleAdminSetActiveBagSelectState() {
  // Use the generic oledDisplayMenu to show available bags
  // The items array needs to be populated from availableBagNames global array
  // For simplicity, we'll create a temporary array of const char* for oledDisplayMenu
  // This is a bit clunky but avoids complex dynamic const char* arrays.
  
  if (redrawOled) {
    oledDisplayMenu("SELECT ACTIVE BAG", availableBagNames, availableBagCount, currentMenuSelection);
  }

  // Handle UP/DOWN for bag selection list
  if (isButtonPressed(BUTTON_A_PIN)) { // UP
    currentMenuSelection = (currentMenuSelection - 1 + availableBagCount) % availableBagCount;
    redrawOled = true;
  } else if (isButtonPressed(BUTTON_B_PIN)) { // DOWN (or Cancel in this context)
    // For this specific menu, Button B will be "Back to Admin Menu"
    Serial.println("Set Active Bag selection cancelled. Returning to Admin Menu.");
    oledShowStatusMessage("Cancelled", "Admin Menu", "", false, 1500);
    currentState = ADMIN_MENU;
    redrawOled = true;
    return;
  }
  // No separate handleButtonInputsForMenu() here, as B is cancel.

  if (isButtonPressed(BUTTON_C_PIN)) { // SELECT action
    String selectedBagID = availableBagIDs[currentMenuSelection];
    String selectedBagName = availableBagNames[currentMenuSelection];
    Serial.printf("Selected Bag: Name=%s, ID=%s\n", selectedBagName.c_str(), selectedBagID.c_str());
    
    if (saveCurrentBagID(selectedBagID, selectedBagName)) {
      oledShowStatusMessage("Active Bag Set:", selectedBagName.substring(0,18), "Fetching list...", true);
      if (fetchEquipmentList_Airtable()) { // Fetch list for the new bag
        // Success message shown by fetchEquipmentList_Airtable
      } else {
        // Error message shown by fetchEquipmentList_Airtable
      }
    } else {
      oledShowStatusMessage("Error Saving Bag", "Config Write Fail", "", false, 3000);
    }
    currentState = ADMIN_MENU; // Return to admin menu
    redrawOled = true;
  }
}

//==============================================================================
// MAIN STATE MACHINE DISPATCHER
//==============================================================================
void runStateMachine() {
  SystemState stateBeforeRun = currentState; 

  switch (currentState) {
    case IDLE_MENU:                         handleIdleMenuState();                      break;
    case REPACK_SESSION_START_CONFIRM:      handleRepackSessionStartConfirmState();     break;
    case SESSION_ACTIVE:                    handleSessionActiveState();                 break;
    case REPACKING_SCAN:                    handleRepackingScanState();                 break;
    case REPACK_CONFIRM_FINISH:             handleRepackConfirmFinishState();           break;
    case REPACK_SESSION_COMPLETE:           handleRepackSessionCompleteState();         break;
    case ADMIN_MODE_UNLOCK:                 handleAdminModeUnlockState();               break;
    case ADMIN_MODE_PREPARE_WIFI:           handleAdminModePrepareWifiState();          break;
    case ADMIN_MENU:                        handleAdminMenuState();                     break;
    case ADMIN_SET_ACTIVE_BAG_FETCH:        handleAdminSetActiveBagFetchState();        break; // NEW
    case ADMIN_SET_ACTIVE_BAG_SELECT:       handleAdminSetActiveBagSelectState();       break; // NEW
    case ADMIN_REPLACE_SCAN_OLD:            handleAdminReplaceScanOldState();           break;
    case ADMIN_REPLACE_SCAN_NEW:            handleAdminReplaceScanNewState();           break;
    case ADMIN_REPLACE_CONFIRM:             handleAdminReplaceConfirmState();           break;
    default:
      Serial.printf("ERROR: Unknown SystemState (%d)! Resetting to IDLE_MENU.\n", currentState);
      currentState = IDLE_MENU;
      currentMenuScreen = MAIN_MENU; // Reset to known safe menu
      currentMenuSelection = 0;
      redrawOled = true;             // Force redraw of the idle menu
      break;
  }

  // If state changed during a handler, reset activity timer and flag OLED for redraw
  if (currentState != stateBeforeRun) {
    Serial.printf("System State changed from %d to %d.\n", stateBeforeRun, currentState);
    lastActivityTime = millis(); // Reset inactivity timer on any state transition
    redrawOled = true;           // Ensure new state's screen is drawn
  }
}

//==============================================================================
// ARDUINO SETUP FUNCTION
//==============================================================================
void setup() {
  Serial.begin(115200);
  // Wait for Serial to initialize, but not indefinitely (e.g., if not connected to PC)
  unsigned long serialStartTime = millis();
  while (!Serial && (millis() - serialStartTime < 2000)) {
    delay(10); 
  }
  delay(1000); // Additional small delay for serial stability

  esp_log_level_set("*", ESP_LOG_WARN); // Reduce default ESP-IDF log verbosity
  Serial.println("\n--- Goalie Gear Tracker (V5.3 - Deep Sleep, K&R Style) ---");

  // Determine the reason for waking up (or power-on)
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  // Initialize core peripherals needed on every boot/wake
  Wire.begin(PN532_SDA, PN532_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println(F("CRITICAL: SSD1306 OLED initialization failed!"));
    // Consider a visual error or halt if display is essential
  } else {
    Serial.println("OLED display initialized OK.");
  }

  setupButtons(); // Configure button GPIO pins

  if (!SPIFFS.begin(true)) { // Mount SPIFFS (true = format if mount fails)
    Serial.println("CRITICAL: SPIFFS Mount Failed!");
  } else {
    Serial.println("SPIFFS initialized OK.");
  }
  
  // Ensure WiFi is in a known, low-power state initially
  WiFi.mode(WIFI_STA);      // Set to Station mode
  WiFi.disconnect(true);    // Disconnect and clear previous session config from RAM
  delay(100);               // Allow WiFi to settle
  Serial.println("WiFi module initialized to disconnected STA mode.");

  // Initialize NFC reader
  nfc.begin();
  uint32_t nfc_firmware_version = nfc.getFirmwareVersion();
  if (!nfc_firmware_version) {
    Serial.println("CRITICAL: PN532 NFC reader not found or failed to initialize. Halting.");
    oledShowStatusMessage("Error:", "NFC FAIL!", "", true); // Show on OLED
    while (1) { // Halt indefinitely
      delay(10); 
    }
  }
  Serial.printf("Found PN5%X NFC chip. Firmware ver. %d.%d\n", 
                (nfc_firmware_version >> 24) & 0xFF, 
                (nfc_firmware_version >> 16) & 0xFF, 
                (nfc_firmware_version >> 8) & 0xFF);
  nfc.SAMConfig(); // Configure Secure Access Module
  Serial.println("NFC Reader Ready.");

  // Restore state or initialize based on wakeup reason
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Wakeup source: External signal (Buttons via RTC_CNTL).");
    if (rtcDataIsValid) {
      Serial.println("Valid RTC data found. Restoring saved application state.");
      currentState = savedCurrentState;
      currentMenuScreen = savedCurrentMenuScreen;
      currentMenuSelection = savedCurrentMenuSelection;
      // rtcDataIsValid remains true; could be cleared if a one-time restore is desired
    } else {
      Serial.println("RTC data marked invalid on wake. Defaulting to IDLE_MENU state.");
      currentState = IDLE_MENU;
      currentMenuScreen = MAIN_MENU;
      currentMenuSelection = 0;
    }
    oledShowStatusMessage("Woke up!", "", "", false, 1000); // Brief "Woke up" message
  } else { // Cold boot or other reset type
    Serial.printf("Wakeup source: %d (Not button-triggered deep sleep wake).\n", wakeup_reason);
    currentState = IDLE_MENU;
    currentMenuScreen = MAIN_MENU;
    currentMenuSelection = 0;
    rtcDataIsValid = false; // Ensure RTC data is marked invalid on a cold boot
    oledShowStatusMessage("Goalie Tracker", "V5.3 Starting...", "", false, 2000);
  }
  
  // Load equipment list from SPIFFS on both cold boot and wake-up.
  // This ensures the list reflects any changes made before a potential sleep.
 if (loadCurrentBagID()) {
    Serial.println("Active bag loaded: " + currentAssignedBagName + " (ID: " + currentAssignedBagID + ")");
  } else {
    Serial.println("No active bag configured on device. Please set one in Admin Menu.");
    // Optionally, display this on OLED briefly if currentState is IDLE_MENU
    if (currentState == IDLE_MENU) {
        //oledShowStatusMessage("No Active Bag", "Set in Admin", "", false, 2500);
        // Be careful with timing here, as IDLE_MENU will redraw soon.
    }
  }
  
  // Load equipment list for the active bag from SPIFFS (if bag is set)
  // The existing loadListFromSPIFFS() will load whatever was last saved.
  // If a new bag was just set and its list fetched, SPIFFS is already up-to-date.
  // If booting and a bag ID is loaded, the SPIFFS list should correspond to it.
  if (!currentAssignedBagID.isEmpty()) {
    loadListFromSPIFFS(); // This loads the equipment for the active bag
    if (currentMaxItems == 0 && wakeup_reason != ESP_SLEEP_WAKEUP_EXT1){
        Serial.println("(Equipment list for active bag is empty in SPIFFS. Use Admin->Fetch.)");
    }
  } else {
      currentMaxItems = 0; // Ensure list is empty if no bag is set
      // Consider clearing the equipment_list.csv or handling this state explicitly
      // SPIFFS.remove(EQUIPMENT_LIST_FILE); // If you want to ensure it's clean
  }

  redrawOled = true;           // Ensure screen is drawn on the first pass of loop()
  lastActivityTime = millis(); // Initialize inactivity timer

  Serial.println("Setup Complete. Initial State: " + String(currentState));
  if (currentMaxItems == 0 && wakeup_reason != ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("(No equipment list loaded from SPIFFS. Use Admin->Fetch.)");
  } else if (currentMaxItems > 0 && currentState == IDLE_MENU) {
    // Optionally, show initial bag status if list is loaded and starting in idle menu
    // displayBagStatusSummaryOLED(); 
    // delay(1500);
    // redrawOled = true; // Ensure menu redraws correctly after status display
  }
}

void loop() {
  runStateMachine();
  yield(); // Allow ESP32 background tasks (like WiFi stack) to run
}