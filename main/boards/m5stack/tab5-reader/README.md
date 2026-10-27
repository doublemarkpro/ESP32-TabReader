# M5Stack Tab5 Reader

Dedicated offline EPUB reader with a separate `m5stack-tab5-reader` OTA identity.
Reuses the Tab5 pin map, ST7121/ST7123 and ILI9881C panel initialization, 8 MHz SD SPI,
touch, battery and RTC. No camera, audio capture, assistant or cloud activation.

Build: `python scripts/build.py m5stack/tab5-reader --name m5stack-tab5-reader` (IDF 6.0.2).
The `-p4x` variant targets the newer P4 silicon; COM7's v1.3 board uses the base variant.
Retains `partitions/han16m.csv`; application-only flashing preserves NVS and TF files.

TF paths remain `/handict/books` and `/handict/fonts`. Existing per-book NVS positions
remain readable. Import modes close fonts and stop card tasks before exporting files.
Safely eject the disk on the host before exiting USB storage mode.
