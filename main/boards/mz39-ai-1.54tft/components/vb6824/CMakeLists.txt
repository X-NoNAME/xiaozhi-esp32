set(incs )
set(srcs )
set(assets )

# Only build on esp32c3: prebuilt blobs and chip HAL in vb6824.c are C3-specific.
if(CONFIG_IDF_TARGET_ESP32C3)
    list(APPEND srcs
        vb6824.c
        )

    list(APPEND incs
        .
        )
endif()

if(CONFIG_VB6824_OTA_SUPPORT AND CONFIG_IDF_TARGET_ESP32C3)
    list(APPEND assets assets/update.html)
    list(APPEND srcs https.c)
endif()

idf_component_register(SRCS "${srcs}"
                            INCLUDE_DIRS "${incs}"
                            EMBED_TXTFILES "${assets}"
                            REQUIRES driver esp_driver_gpio esp_driver_uart esp_ringbuf esp_wifi esp_http_client esp_http_server espressif__cjson esp_timer)



if(CONFIG_IDF_TARGET_ESP32)
    set(lib_target "esp32")
elseif(CONFIG_IDF_TARGET_ESP32S2)
    set(lib_target "esp32s2")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    set(lib_target "esp32s3")
elseif(CONFIG_IDF_TARGET_ESP32C2)
    set(lib_target "esp32c2")
elseif(CONFIG_IDF_TARGET_ESP32C3)
    set(lib_target "esp32c3")
elseif(CONFIG_IDF_TARGET_ESP32C5)
    set(lib_target "esp32c5")
endif()


if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.5")
    set(lib_version "idf5.5")
else()
    set(lib_version "idf5.4")
endif()

if(NOT CONFIG_IDF_TARGET_ESP32C3)
    return()
endif()

set(lib_path "libs/${lib_version}/lib${lib_target}.a")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${lib_path}")
    add_prebuilt_library(vbota "${lib_path}")
    target_link_libraries(${COMPONENT_LIB} PRIVATE vbota)
endif()