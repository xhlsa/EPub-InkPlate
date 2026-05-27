# littlefs_root — LittleFS partition source (Phase 2)

This directory is the source tree for the `storage` LittleFS partition.
CMake builds it into `build_single/storage.bin` automatically when
`BUILD_VARIANT=single_book`.

## Required layout

```
littlefs_root/
  fonts_list.xml          ← copy from your SD card root
  books/
    YourBook.epub         ← the book to read (one .epub)
  fonts/
    *.ttf / *.otf         ← font files listed in fonts_list.xml
```

The `.locs` pagination cache (`books/YourBook.locs`) is generated
automatically on first boot when not present; it is written back to
LittleFS. Subsequent boots skip pagination and load instantly.

## Populating

Copy files from your existing SD card:

```bash
cp /path/to/sdcard/fonts_list.xml  littlefs_root/
cp /path/to/sdcard/books/*.epub    littlefs_root/books/
cp /path/to/sdcard/fonts/*.ttf     littlefs_root/fonts/
cp /path/to/sdcard/fonts/*.otf     littlefs_root/fonts/  2>/dev/null || true
```

## Building and flashing

```bash
# Build app + LittleFS image
idf.py -C . -B build_single -D BUILD_VARIANT=single_book \
       -D DEVICE=INKPLATE_5V2 -D APP_VERSION=2.2.0 build

# Flash app only (does NOT touch the storage partition)
idf.py -C . -B build_single -D BUILD_VARIANT=single_book \
       -D DEVICE=INKPLATE_5V2 -D APP_VERSION=2.2.0 \
       -p /dev/ttyUSB0 app-flash

# Flash storage partition only (update book/fonts without reflashing app)
esptool.py --chip esp32 -p /dev/ttyUSB0 \
           write_flash 0x310000 build_single/storage.bin

# Flash everything (first-time setup)
idf.py -C . -B build_single -D BUILD_VARIANT=single_book \
       -D DEVICE=INKPLATE_5V2 -D APP_VERSION=2.2.0 \
       -p /dev/ttyUSB0 flash
esptool.py --chip esp32 -p /dev/ttyUSB0 \
           write_flash 0x310000 build_single/storage.bin
```

## .gitignore note

`books/` and `fonts/` are gitignored (binary content, not source).
`fonts_list.xml` is tracked if you want reproducible builds.
