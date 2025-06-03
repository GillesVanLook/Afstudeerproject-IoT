// Config.h
#ifndef CONFIG_H
#define CONFIG_H

// --- Hardware Pin Definitions ---
// PN532 NFC Reader
#define PN532_SDA                   21
#define PN532_SCL                   22

// Buttons (ensure these match your wiring)
#define BUTTON_A_PIN                4       // Up / Next / Select / Option 1 / Yes
#define BUTTON_B_PIN                12      // Down / Back / Cancel / Option 3 (Admin) / No / Manual Finish
#define BUTTON_C_PIN                13      // Enter / Confirm

// --- OLED Display Configuration ---
#define SCREEN_WIDTH                128
#define SCREEN_HEIGHT               64
#define OLED_RESET                  -1      // Reset pin # (or -1 if sharing Arduino reset pin)
#define OLED_I2C_ADDRESS            0x3C    // Common address, verify for your display

// --- Application Behavior & Timings ---
#define MAX_EXPECTED_ITEMS          20      // Max items in an equipment list
#define MAX_BAGS_TO_LIST            10      // Max bags to fetch/display in "Set Active Bag" menu

#define NFC_POLLING_INTERVAL_MS     200     // How often to check for an NFC tag
#define TAG_READ_DELAY_MS           500     // Pause after a successful tag read to prevent immediate re-read
#define HTTP_TIMEOUT_MS             10000   // Timeout for WiFi/HTTP requests (milliseconds)
#define ADMIN_TAG_SCAN_TIMEOUT_MS   10000   // How long to wait for admin tag scan before timing out
// #define ADMIN_LONG_PRESS_MS         2000 // Currently unused, but could be for future features

#define STATUS_MESSAGE_DURATION_MS  2000    // Default duration for temporary OLED status messages
#define WELL_DONE_TIMEOUT_MS        5000    // Auto-return from "Well Done" / session complete screen

#define DEBOUNCE_DELAY_MS           50      // Button debounce delay

// --- Deep Sleep Configuration ---
#define DEEP_SLEEP_TIMEOUT_MS       60000   // Inactivity duration before entering deep sleep (e.g., 60 seconds)
// BUTTON_MASK is derived from BUTTON_x_PINs in the main .ino, so it stays there or is moved carefully.

// --- File System Paths ---
#define EQUIPMENT_LIST_FILE         "/equipment_list.csv" // SPIFFS path for cached equipment list
#define BAG_CONFIG_FILE             "/bag_config.txt"     // SPIFFS path for active bag configuration

// --- Debugging & Logging ---
// You could add flags here to enable/disable certain verbose logging sections
// #define DEBUG_NFC_VERBOSE
// #define DEBUG_HTTP_VERBOSE

#endif // CONFIG_H