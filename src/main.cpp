#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_partition.h>
// #include <utils.h>
// #include <fonts/consolaUkr6.h> 

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET (-1)
#define ENABLE_CRASH_TEST 0

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void printBinary(const uint8_t *data, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (index % 16 == 0) {
      Serial.println();
    }
    if (data[index] < 0x10) {
      Serial.print('0');
    }
    Serial.print(data[index], HEX);
    Serial.print(' ');
  }
  Serial.println();
}

void printPartitionTable() {
  Serial.println();
  Serial.println(F("Partitions discovered by ESP-IDF:"));

  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_ANY,
      ESP_PARTITION_SUBTYPE_ANY,
      nullptr);

  if (iterator == nullptr) {
    Serial.println(F("No partitions found."));
    return;
  }

  uint8_t rawData[32];
  uint32_t partitionNumber = 0;

  while (iterator != nullptr) {
    const esp_partition_t *partition = esp_partition_get(iterator);
    uint32_t endAddress = partition->address + partition->size;
    uint32_t lastAddress = endAddress - 1;
    Serial.printf(
        "Partition %lu: label=\"%s\" type=0x%02X subtype=0x%02X "
      "start=0x%08lX end=0x%08lX "
      "range=[0x%08lX..0x%08lX] size=0x%08lX (%lu bytes) encrypted=%s\n",
        partitionNumber++, partition->label, partition->type,
      partition->subtype, partition->address, endAddress,
      partition->address, lastAddress, partition->size,
        partition->size, partition->encrypted ? "yes" : "no");

    esp_err_t result = esp_partition_read(partition, 0, rawData, sizeof(rawData));
    if (result == ESP_OK) {
      Serial.print(F("First 32 bytes:"));
      printBinary(rawData, sizeof(rawData));
    } else {
      Serial.printf("Read failed: %s\n", esp_err_to_name(result));
    }

    iterator = esp_partition_next(iterator);
  }

  esp_partition_iterator_release(iterator);
}

bool scanI2C() {
  bool found = false;
  Serial.println(F("Scanning I2C bus..."));

  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      Serial.printf("I2C device found at 0x%02X\n", addr);
      if (addr == 0x3C || addr == 0x3D) {
        found = true;
      }
    }
  }

  return found;
}

bool initDisplayOnPins(int sdaPin, int sclPin) {
  Wire.begin(sdaPin, sclPin);
  Serial.printf("Trying OLED on SDA=%d, SCL=%d\n", sdaPin, sclPin);

  if (!scanI2C()) {
    Serial.println(F("No I2C device found on this pin pair."));
    return false;
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 init failed on this pin pair."));
    return false;
  }

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("TTGO OLED startup"));
  printPartitionTable();

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Hello, World!");

  bool ready = false;

  // Common TTGO LoRa32 V2 pinout
  ready = initDisplayOnPins(21, 22);

  // Fallback for TTGO LoRa32 V1/V1.1
  if (!ready) {
    ready = initDisplayOnPins(4, 15);
  }

  if (!ready) {
    Serial.println(F("OLED not found. Check board variant and wiring."));
    for (;;) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.display();
  display.setTextSize(2);
  display.setCursor(0, 10);
  display.println("Smert' rusni. Slava Ukraini!");
  display.display();

#if ENABLE_CRASH_TEST
  Serial.println(F("Intentional invalid memory access in 3 seconds..."));
  delay(3000);
  volatile uint32_t *invalidAddress = nullptr;
  *invalidAddress = 0xDEADBEEF;
#endif
}

void loop() {
  
}