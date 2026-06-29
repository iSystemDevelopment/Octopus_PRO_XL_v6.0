# Third-party notices — Octopus PRO XL

Copyright (c) 2026 DIODAC ELECTRONICS / iSystem.  
Proprietary Octopus PRO XL **Software** is licensed under [LICENSE](./LICENSE).  
This file lists **third-party** components only.

---

## Firmware (Arduino / ESP-IDF)

Built with the **Arduino ESP32** core and **ESP-IDF** (Espressif).  
Governed by their respective licenses (Apache 2.0 / LGPL components as applicable).  
Obtain source and license texts from your installed board package and Espressif SDK.

| Component | Typical use in project | License (check your install) |
|-----------|------------------------|------------------------------|
| ESP32 Arduino core / ESP-IDF | MCU, FreeRTOS, drivers, I2S | Espressif / mixed |
| ESP32Encoder | Rotary encoder | Project license on package |
| Adafruit GFX Library | OLED drawing | BSD |
| Adafruit SH110X | SH1106 display driver | BSD |

When you distribute **compiled firmware**, comply with obligations of embedded
third-party libraries (attribution, source offers where required by LGPL, etc.).

---

## OctopusApp.html (browser)

| Component | Delivery | License |
|-----------|----------|---------|
| Font Awesome 6 | CDN (jsDelivr / cdnjs) | Font Awesome Free License |
| Orbitron, JetBrains Mono, etc. | Google Fonts CDN | SIL Open Font License |

No server-side runtime — static HTML + Web MIDI API.

---

## octopus_web.html (product site)

| Component | Delivery | License |
|-----------|----------|---------|
| Tailwind CSS | CDN | MIT |
| Font Awesome 6 | CDN | Font Awesome Free License |
| Google Fonts (Inter, Orbitron, JetBrains Mono) | CDN | SIL Open Font License |

Images (`logo.jpg`, `octopus-app-hero.jpg`, etc.) are **proprietary** product
assets unless marked otherwise — not covered by third-party licenses above.

---

## Documentation examples

Factory JSON under `examples/octopusapp/` is tutorial data for OctopusApp.
Use with the product; redistribution as a competing sound/pattern library is
not permitted without permission from DIODAC ELECTRONICS / iSystem.

---

For permission to use Octopus proprietary materials: **diodac.electronics@gmail.com**
