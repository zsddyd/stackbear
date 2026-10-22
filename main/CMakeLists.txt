idf_component_register(
    SRCS "main.c"
         "sb_config.c"
         "sb_cache.c"
         "sb_cache_slot.c"
         "sb_json.c"
         "sb_nav.c"
         "sb_text.c"
         "sb_version.c"
         "sb_ota.c"
         "sb_diag.c"
         "sb_proto.c"
         "sb_vad.c"
         "sb_adpcm.c"
         "sb_net.c"
         "sb_net_web.c"
         "sb_uplink.c"
         "sb_recorder.c"
         "sb_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_server esp_hw_support esp_netif esp_timer esp_wifi lwip nvs_flash
             app_update mbedtls
)

# --- 构建信息注入(版本页显示 git 提交与编译时间)--------------------------------
# 这些值被 main/sb_version.h 使用。取不到 git 时用 unknown,不让构建失败。
#
# 提交短号统一用 8 位,**由 git 自己保证唯一**(`--short=8` 的语义是"至少 8 位、
# 且唯一":万一 8 位不够区分,git 会自动加长)。不要把这里换成手工截断 ——
# tools/make_ota_manifest.py 也用 `--short=8`,两边必须拿到同一个字符串,否则
# 设备会把每个版本都当成新版本,每次检查都重刷一遍。
find_package(Git QUIET)
set(SB_GIT_COMMIT "unknown")
set(SB_GIT_BRANCH "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
                    OUTPUT_VARIABLE SB_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
        string(STRIP "${SB_GIT_COMMIT}" SB_GIT_COMMIT)
    else()
        set(SB_GIT_COMMIT "unknown")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
                    OUTPUT_VARIABLE SB_GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    string(STRIP "${SB_GIT_BRANCH}" SB_GIT_BRANCH)
endif()
string(TIMESTAMP SB_BUILD_TIME "%Y-%m-%d %H:%M UTC" UTC)
set(SB_APP_VERSION "0.7.10")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SB_GIT_COMMIT="${SB_GIT_COMMIT}"
    SB_GIT_BRANCH="${SB_GIT_BRANCH}"
    SB_BUILD_TIME="${SB_BUILD_TIME}"
    SB_APP_VERSION="${SB_APP_VERSION}"
)
