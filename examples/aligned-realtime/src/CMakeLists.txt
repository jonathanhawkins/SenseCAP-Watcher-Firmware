file(GLOB_RECURSE UI_SRCS ./ui/*.c)

# Aligned Tools client + LiveKit SDK (proper signaling, no WHIP)
set(COMMON_SRC "main.cpp" "aligned_client.cpp" "livekit_room.c")

if(IDF_TARGET STREQUAL linux)
	idf_component_register(
		SRCS ${COMMON_SRC}
		REQUIRES livekit__livekit esp_http_client json)
else()
	idf_component_register(
		SRCS ${COMMON_SRC} "wifi.cpp" "media.c" "board.c" "cmd.cpp" "pm.c" "volume_control.c" ${UI_SRCS}
		REQUIRES driver esp_wifi nvs_flash esp_psram esp_http_client console json livekit__livekit
		         esp_codec_dev esp_io_expander esp_lcd_spd2010 esp_lcd_touch_spd2010
		         tempotian__media_lib_sal tempotian__av_render espressif__esp_capture
		INCLUDE_DIRS "." "./ui")
endif()
