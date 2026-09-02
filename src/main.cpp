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

constexpr uint8_t BUTTON_PIN = 0;
constexpr uint8_t LORA_CS = 18;
constexpr uint8_t LORA_DIO0 = 26;
constexpr uint8_t LORA_RST = 14;
constexpr uint8_t LORA_DIO1 = 33;
constexpr float LORA_FREQUENCY = 868.0;
constexpr TickType_t DOUBLE_CLICK_WINDOW = pdMS_TO_TICKS(350);
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);

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
          while (xTaskGetTickCount() - releaseTime < DOUBLE_CLICK_WINDOW) {
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

void radioTask(void *parameter) {
  (void)parameter;
  int16_t status = radio.begin(LORA_FREQUENCY);
  if (status == RADIOLIB_ERR_NONE) {
    Serial.println(F("Radio task: SX1276 ready"));
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

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("Button -> Queue -> Main Loop -> Radio Task"));

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

void loop() {
  ButtonPress press;
  if (xQueueReceive(buttonQueue, &press, portMAX_DELAY) != pdTRUE) {
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