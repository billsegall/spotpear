idf_component_register(SRCS "play_audio_main.c"
                       INCLUDE_DIRS ".")

# Build the SPIFFS image from main/audio_fs/ and flash it with the app in the
# same `idf.py flash` pass. The audio blob (audio_fs/audio.pcm) is produced by
# tools/fetch_audio.sh and is gitignored — copyrighted, kept local.
spiffs_create_partition_image(assets "${CMAKE_CURRENT_SOURCE_DIR}/audio_fs" FLASH_IN_PROJECT)
