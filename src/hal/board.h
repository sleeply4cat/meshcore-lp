// Faketec v4 (ProMicro nRF52840 + Heltec HT-RA62 / SX1262 with 1.8V TCXO on DIO3)
#pragma once

#define PIN(port, pin)   ((port) * 32 + (pin))

// SX1262
#define PIN_LORA_NSS     PIN(1, 13)
#define PIN_LORA_SCK     PIN(1, 11)
#define PIN_LORA_MOSI    PIN(1, 15)
#define PIN_LORA_MISO    PIN(0, 2)
#define PIN_LORA_BUSY    PIN(0, 29)
#define PIN_LORA_DIO1    PIN(0, 10)
#define PIN_LORA_RESET   PIN(0, 9)
// HT-RA62 pin 11 "RXEN": through R4 (10k) onto Vcont1 of the UPG2179 RF switch, which is otherwise only
// pulled up by 1 Mohm. Stock MeshCore (RadioLib setRfSwitchPins) drives it high while receiving; left
// floating the RX path loses sensitivity. High in RX/idle, low while transmitting.
#define PIN_LORA_RXEN    PIN(0, 17)

// ProMicro "VCC" output switch, the LoRa module is powered from it -> must stay on.
#define PIN_VCC_EN       PIN(0, 13)

#define PIN_LED          PIN(0, 15)   // active high
#define PIN_BUTTON       PIN(1, 0)    // active low (optional, fitted on v3/v4 boards)
#define PIN_VBAT         PIN(0, 31)   // AIN7, divider to battery
#define VBAT_AIN         7

// Faketec v4 MOSFET switched outputs (have pull-downs on the PCB): keep them low.
#define PIN_MOSFET1      PIN(0, 24)
#define PIN_MOSFET2      PIN(0, 6)
#define PIN_MOSFET3      PIN(0, 8)

// SX1262 TCXO supply from DIO3 (HT-RA62: 1.8V)
#define SX126X_TCXO_VOLTAGE_CODE  0x02   // 0x02 = 1.8V
