# Technische Primaerquellen

Eigener Firmwarekern; keine Uebernahme aus cer2nut oder Bauerino.

- ESP-IDF USB Host: https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/api-reference/peripherals/usb_host.html
- Espressif CP210x-Treiber: https://components.espressif.com/components/espressif/usb_host_cp210x_vcp/versions/2.2.0
- Espressif CDC-ACM: https://components.espressif.com/components/espressif/usb_host_cdc_acm/versions/2.1.0
- Silicon-Labs CP210x-Kennungen und GET_PARTNUM: https://github.com/torvalds/linux/blob/master/drivers/usb/serial/cp210x.c
- ESP-IDF NimBLE: https://github.com/espressif/esp-idf/tree/v5.3/examples/bluetooth/nimble
- Waveshare S3-Zero: https://www.waveshare.com/wiki/ESP32-S3-Zero
- Espressif S3-WROOM: https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf
- Webflasher: https://esphome.github.io/esp-web-tools/

CP210x/CDC-ACM und ESP-IDF werden als offizielle Abhaengigkeiten mit ihren
eigenen Lizenzhinweisen eingebunden. Zentrale Lizenztexte liegen unter third_party/LICENSES. Weitere komponentenbezogene
Hinweise bleiben in den originalen SDK- und Komponentenpaketen erhalten.

Entscheidungen28.10.2026: USB-Chip statt LED-Probe; niemals feste Figuren-IDs
oder Praefixe. Sentio bleibt ein Besetzungsbrett. Keine dauerhafte Kalibrierung
im Interface. Einlernen und Figurenbedeutung bestimmt die Schachanwendung.
