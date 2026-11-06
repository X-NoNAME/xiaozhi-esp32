#ifndef ASSETS_PNG_H
#define ASSETS_PNG_H

#include <cstddef>
#include <cstdint>

// The server asset packs carry emoji as PNG files. LVGL can only draw PNG
// through lv_lodepng which decodes the whole image into RAM (ARGB8888 plus a
// sliding decompression window); boards without PSRAM (e.g. ESP32-C3) do not
// have the heap for that, so the emoji silently never show.
//
// This module converts those PNGs once into LVGL RGB565A8 planes and stores
// them in the unused tail of the assets partition, together with a CRC-tagged
// directory sector. The converted planes are memory-mapped and drawn directly,
// costing zero heap at run time. Conversion needs only a few KB of RAM
// (one PNG row at a time); the 32 KiB deflate history window is satisfied by
// reading back the partially written raw stream from flash.
//
// A ~10 s one-time flash conversion is expected on the first boot after an
// asset update; every later boot validates the directory by CRC only
// (milliseconds). The interface intentionally uses opaque pointers so the
// header compiles in host tests without ESP-IDF/LVGL headers.

// Part of the assets partition mmap that Assets::LvglStrategy provides (const
// uint8_t*), and the esp_partition_t* of the assets partition.
int AssetsPngEnsureCache(const uint8_t* mmap_root, const void* partition);

// Invalidates the in-RAM registry (called when the assets partition is unmapped).
void AssetsPngReset();

// Returns an opaque lv_img_dsc_t* (cast at the caller) for the cached
// converted image of `name`, or nullptr when the name is not in the cache.
// The descriptor stays valid until the next AssetsPngEnsureCache cycle.
const void* AssetsPngGet(const char* name);

#endif  // ASSETS_PNG_H
