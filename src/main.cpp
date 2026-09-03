#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RadioLib.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET (-1)

// Апаратна конфігурація LoRa-модуля, яку можна перевизначити для іншої плати.
#define LORA_CS 18
#define LORA_DIO0 26
#define LORA_DIO1 33
#define LORA_RST 23
// Потужність TX у dBm: можливі приклади 2, 5, 10, 14 або 17; максимум залежить від модуля та норм регіону.
#define LORA_TX_POWER 2
#define LORA_FREQUENCY 868.0
#define LORA_BUTTON_PIN 0
#define RADIO_PACKET_SIZE 64
#define SINGLE_RADIO_PACKET_SIZE 32
// Максимальна частка ефірного часу: після передачі радіо очікує 99 тривалостей TX.
#define LORA_DUTY_CYCLE_PERCENT 1

constexpr char SINGLE_BUTTON_COMMAND[] = "singleBtn";
constexpr char DOUBLE_BUTTON_COMMAND[] = "doubleBtn";

// Якщо друге натискання відбулося до цього порогу, формується doubleBtn;
// після порогу натискання вважаються двома окремими singleBtn.
#define DOUBLE_CLICK_THRESHOLD_MS 350

// Час debounce сталий, а поріг подвійного натискання можна змінити через Serial.
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);
uint32_t doubleClickWindowMs = DOUBLE_CLICK_THRESHOLD_MS;

enum class ButtonPress : uint8_t {
  Single,
  Double
};

struct RadioPacket {
  ButtonPress type;
  char text[RADIO_PACKET_SIZE - sizeof(ButtonPress)];
};

static_assert(sizeof(RadioPacket) == RADIO_PACKET_SIZE,
              "RadioPacket must be exactly 64 bytes");
static_assert(SINGLE_RADIO_PACKET_SIZE < RADIO_PACKET_SIZE,
              "Single packet must be smaller than double packet");
static_assert(LORA_DUTY_CYCLE_PERCENT > 0 && LORA_DUTY_CYCLE_PERCENT <= 100,
              "LORA_DUTY_CYCLE_PERCENT must be between 1 and 100");

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
QueueHandle_t buttonQueue;
QueueHandle_t radioQueue;
uint32_t sentPacketCount = 0;

// Показує на OLED поточний стан і розмір радіопакета.
void showRadioStatus(size_t packetSize, const char *status, uint32_t elapsedMs = 0) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (packetSize > 0) {
    display.printf("%u byte packet %s", static_cast<unsigned>(packetSize), status);
  } else {
    display.println(F("Button radio ready"));
  }

  display.setCursor(0, 16);
  display.printf("Total sent: %lu", static_cast<unsigned long>(sentPacketCount));

  if (elapsedMs > 0) {
    display.setCursor(0, 32);
    display.printf("Time: %lu ms", static_cast<unsigned long>(elapsedMs));
  }

  display.display();
}

// Задача опитує кнопку, усуває дребезг і розрізняє одинарне/подвійне натискання.
void buttonTask(void *parameter) {
  (void)parameter;
  bool previousState = digitalRead(LORA_BUTTON_PIN);

  for (;;) {
    bool currentState = digitalRead(LORA_BUTTON_PIN);

    if (previousState == HIGH && currentState == LOW) {
      vTaskDelay(BUTTON_DEBOUNCE);
      if (digitalRead(LORA_BUTTON_PIN) == LOW) {
        while (digitalRead(LORA_BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        vTaskDelay(BUTTON_DEBOUNCE);
        ButtonPress press = ButtonPress::Single;

        if (digitalRead(LORA_BUTTON_PIN) == HIGH) {
          TickType_t releaseTime = xTaskGetTickCount();
          while (xTaskGetTickCount() - releaseTime < pdMS_TO_TICKS(doubleClickWindowMs)) {
            if (digitalRead(LORA_BUTTON_PIN) == LOW) {
              vTaskDelay(BUTTON_DEBOUNCE);
              if (digitalRead(LORA_BUTTON_PIN) == LOW) {
                press = ButtonPress::Double;
                while (digitalRead(LORA_BUTTON_PIN) == LOW) {
                  vTaskDelay(pdMS_TO_TICKS(10));
                }
                break;
              }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
          }
        }

        xQueueSend(buttonQueue, &press, portMAX_DELAY);
      }
    }

    previousState = currentState;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Задача ініціалізує SX1276 і відправляє пакети з radioQueue через LoRa.
void radioTask(void *parameter) {
  (void)parameter;
  int16_t status = radio.begin(LORA_FREQUENCY);
  if (status == RADIOLIB_ERR_NONE) {
    int16_t powerStatus = radio.setOutputPower(LORA_TX_POWER);
    if (powerStatus == RADIOLIB_ERR_NONE) {
      Serial.printf("Radio task: SX1276 ready, TX power=%d dBm\n", LORA_TX_POWER);
    } else {
      Serial.printf("Radio task: TX power setup failed, code %d\n", powerStatus);
    }
  } else {
    Serial.printf("Radio task: init failed, code %d\n", status);
  }

  RadioPacket packet;
  for (;;) {
    if (xQueueReceive(radioQueue, &packet, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (status != RADIOLIB_ERR_NONE) {
      Serial.println(F("Radio task: packet skipped, radio unavailable"));
      continue;
    }

    size_t packetSize = packet.type == ButtonPress::Single
        ? SINGLE_RADIO_PACKET_SIZE
        : RADIO_PACKET_SIZE;
    uint32_t transmissionStart = millis();
    int16_t sendStatus = radio.transmit(
      reinterpret_cast<const uint8_t *>(&packet),
        packetSize);
    uint32_t transmissionTime = millis() - transmissionStart;
    if (sendStatus == RADIOLIB_ERR_NONE) {
      ++sentPacketCount;
    }
    showRadioStatus(packetSize, sendStatus == RADIOLIB_ERR_NONE ? "sent" : "failed",
                    transmissionTime);
    Serial.printf("Radio task: sent \"%s\", size=%d bytes, result=%d\n",
            packet.text,
            packetSize,
            sendStatus);

    // Для duty cycle 1% пауза становить 99 тривалостей попередньої передачі.
    uint32_t quietTimeMs = transmissionTime *
        (100 - LORA_DUTY_CYCLE_PERCENT) / LORA_DUTY_CYCLE_PERCENT;
    if (quietTimeMs > 0) {
      Serial.printf("Radio task: duty-cycle wait %lu ms\n",
                    static_cast<unsigned long>(quietTimeMs));
      vTaskDelay(pdMS_TO_TICKS(quietTimeMs));
    }
  }
}

// Головна функція ініціалізує дисплей, створює черги та запускає задачі.
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("Button -> Queue -> Main Loop -> Radio Task"));
  // Приклади: singleBtn; doubleBtn; doubleBtn 1000, потім Enter.
  // doubleBtn без аргументу імітує double, а аргумент задає інтервал між натисканнями.
  Serial.printf("Serial commands: %s, %s [100..2000]\n",
                SINGLE_BUTTON_COMMAND, DOUBLE_BUTTON_COMMAND);

  pinMode(LORA_BUTTON_PIN, INPUT_PULLUP);
  buttonQueue = xQueueCreate(5, sizeof(ButtonPress));
  radioQueue = xQueueCreate(5, sizeof(RadioPacket));

  if (buttonQueue == nullptr || radioQueue == nullptr) {
    Serial.println(F("Queue creation failed"));
    while (true) {
      delay(1000);
    }
  }

  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    showRadioStatus(0, "ready");
  }

  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(radioTask, "RadioTask", 4096, nullptr, 1, nullptr, 0);
}

// Обробляє команди з Serial Monitor і передає їх у ту саму чергу, що й кнопка.
void processSerialCommands() {
  static char command[16];
  static uint8_t commandLength = 0;

  while (Serial.available() > 0) {
    char character = static_cast<char>(Serial.read());

    if (character == '\r') {
      continue;
    }

    if (character == '\n') {
      command[commandLength] = '\0';

      unsigned long requestedWindowMs = 0;

      if (strcmp(command, SINGLE_BUTTON_COMMAND) == 0) {
        ButtonPress press = ButtonPress::Single;
        xQueueSend(buttonQueue, &press, 0);
        Serial.println(F("Serial: simulated single press"));
      } else if (strcmp(command, DOUBLE_BUTTON_COMMAND) == 0) {
        ButtonPress press = ButtonPress::Double;
        xQueueSend(buttonQueue, &press, 0);
        Serial.println(F("Serial: simulated double press"));
      } else if (strncmp(command, DOUBLE_BUTTON_COMMAND,
                         strlen(DOUBLE_BUTTON_COMMAND)) == 0 &&
                 command[strlen(DOUBLE_BUTTON_COMMAND)] == ' ' &&
                 sscanf(command + strlen(DOUBLE_BUTTON_COMMAND) + 1,
                        "%lu", &requestedWindowMs) == 1) {
        if (requestedWindowMs >= 100 && requestedWindowMs <= 2000) {
          if (requestedWindowMs > doubleClickWindowMs) {
            ButtonPress singlePress = ButtonPress::Single;
            xQueueSend(buttonQueue, &singlePress, 0);
            xQueueSend(buttonQueue, &singlePress, 0);
            Serial.printf("Serial: %lu ms > %lu ms, queued two single presses\n",
                          requestedWindowMs, doubleClickWindowMs);
          } else {
            ButtonPress doublePress = ButtonPress::Double;
            xQueueSend(buttonQueue, &doublePress, 0);
            Serial.printf("Serial: %lu ms <= %lu ms, queued one double press\n",
                          requestedWindowMs, doubleClickWindowMs);
          }
        } else {
          Serial.println(F("doubleBtn must be between 100 and 2000 ms"));
        }
      } else if (commandLength > 0) {
        Serial.printf("Use %s, %s, or %s 100..2000\n",
                      SINGLE_BUTTON_COMMAND, DOUBLE_BUTTON_COMMAND,
                      DOUBLE_BUTTON_COMMAND);
      }

      commandLength = 0;
      continue;
    }

    if (commandLength < sizeof(command) - 1) {
      command[commandLength++] = character;
    }
  }
}

// Головний цикл отримує тип натискання та формує відповідний радіопакет.
void loop() {
  processSerialCommands();

  ButtonPress press;
  if (xQueueReceive(buttonQueue, &press, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  RadioPacket packet{};
  packet.type = press;
  const char *message = press == ButtonPress::Single
      ? "BUTTON SINGLE"
      : "BUTTON DOUBLE";
  strncpy(packet.text, message, sizeof(packet.text) - 1);
  packet.text[sizeof(packet.text) - 1] = '\0';

  size_t packetSize = press == ButtonPress::Single
      ? SINGLE_RADIO_PACKET_SIZE
      : RADIO_PACKET_SIZE;
  showRadioStatus(packetSize, "sending...");
  Serial.printf("Main loop: %s\n", packet.text);
  xQueueSend(radioQueue, &packet, portMAX_DELAY);
}