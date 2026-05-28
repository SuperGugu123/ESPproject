set(srcs
    INMP441/inmp441.c
    LCD/lcd.c
    LCD/mylvgl.c
    MQTT/app_mqtt.c
    INMP441/app_sr.c
    SG90/sg90.c
    RADAR/radar.c
    OTA/app_ota.c
    WIFI/wifista.c
    WIFI/wificsi.c)

set(include_dirs
    INMP441
    SG90
    RADAR
    LCD
    MQTT
    OTA
    WIFI)     
    
set(requires
    app_update
    esp_http_client
    esp_driver_gpio
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_spi
    esp_event
    esp_lcd
    esp_lcd_ili9341
    esp_lcd_touch_xpt2046
    lvgl
    esp_netif
    esp_timer
    esp_wifi
    mqtt
    nvs_flash
    esp-radar
    espressif__esp-sr
    lwip)     

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs} 
                       REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
