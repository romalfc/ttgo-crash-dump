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
#define LORA_CS 18
#define LORA_RST 23
// Потужність TX у dBm: можливі приклади 2, 5, 10, 14 або 17; максимум залежить від модуля та норм регіону.
#define LORA_TX_POWER 2

constexpr uint8_t BUTTON_PIN = 0;
constexpr uint8_t LORA_DIO0 = 26;
constexpr uint8_t LORA_DIO1 = 33;
constexpr float LORA_FREQUENCY = 868.0;
constexpr char SINGLE_BUTTON_COMMAND[] = "singleBtn";
constexpr char DOUBLE_BUTTON_COMMAND[] = "doubleBtn";

// Якщо друге натискання відбулося до цього порогу, формується doubleBtn;
// після порогу натискання вважаються двома окремими singleBtn.
#define DOUBLE_CLICK_THRESHOLD_MS 350

// Час debounce сталий, а поріг подвійного натискання можна змінити через Serial.
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);
volatile uint32_t doubleClickWindowMs = DOUBLE_CLICK_THRESHOLD_MS;

enum class ButtonPress : uint8_t {
  Single,
  Double
};

struct RadioPacket {
  ButtonPress type;
  char text[32];
};

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
QueueHandle_t buttonQueue;
QueueHandle_t radioQueue;

// Задача опитує кнопку, усуває дребезг і розрізняє одинарне/подвійне натискання.
void buttonTask(void *parameter) {
  (void)parameter;
  bool previousState = digitalRead(BUTTON_PIN);

  for (;;) {
    bool currentState = digitalRead(BUTTON_PIN);

    if (previousState == HIGH && currentState == LOW) {
      vTaskDelay(BUTTON_DEBOUNCE);
      if (digitalRead(BUTTON_PIN) == LOW) {
        while (digitalRead(BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        vTaskDelay(BUTTON_DEBOUNCE);
        ButtonPress press = ButtonPress::Single;

        if (digitalRead(BUTTON_PIN) == HIGH) {
          TickType_t releaseTime = xTaskGetTickCount();
          while (xTaskGetTickCount() - releaseTime < pdMS_TO_TICKS(doubleClickWindowMs)) {
            if (digitalRead(BUTTON_PIN) == LOW) {
              vTaskDelay(BUTTON_DEBOUNCE);
              if (digitalRead(BUTTON_PIN) == LOW) {
                press = ButtonPress::Double;
                while (digitalRead(BUTTON_PIN) == LOW) {
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

    int16_t sendStatus = radio.transmit(packet.text);
    Serial.printf("Radio task: sent \"%s\", result=%d\n", packet.text, sendStatus);
  }
}

// Головна функція ініціалізує дисплей, створює черги та запускає задачі.
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("Button -> Queue -> Main Loop -> Radio Task"));
  // Приклади: singleBtn; doubleBtn; doubleBtn 500, потім Enter.
  // doubleBtn без аргументу імітує подвійне натискання.
  Serial.printf("Serial commands: %s, %s [100..2000]\n",
                SINGLE_BUTTON_COMMAND, DOUBLE_BUTTON_COMMAND);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
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
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Button radio ready"));
    display.display();
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
          doubleClickWindowMs = requestedWindowMs;
          Serial.printf("Serial: double-click window=%lu ms\n", doubleClickWindowMs);
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

  RadioPacket packet;
  packet.type = press;
  const char *message = press == ButtonPress::Single
      ? "BUTTON SINGLE"
      : "BUTTON DOUBLE";
  strncpy(packet.text, message, sizeof(packet.text) - 1);
  packet.text[sizeof(packet.text) - 1] = '\0';

  Serial.printf("Main loop: %s\n", packet.text);
  xQueueSend(radioQueue, &packet, portMAX_DELAY);
}