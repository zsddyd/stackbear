cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(FoloToy-AI-Passport)

# --- libopus 临时内存模式修正 ------------------------------------------------
# 78/esp-opus 自带 -DUSE_ALLOCA,而 libopus 的 celt/stack_alloc.h 分支顺序是
#   #if VAR_ARRAYS  #elif USE_ALLOCA  #else(伪栈)  #endif
# 只要 USE_ALLOCA 还在,编码时 CELT/SILK 的临时缓冲区就全部用 alloca 落在【任务栈】上
# (16 kHz / 60 ms 需要几十 KB),在无 PSRAM 的 ESP32-C3 上必然把录音任务栈撑爆
# (已用串口确认:Stack protection fault in task "sb_rec")。
# CMake 无法用 -U 稳定覆盖组件自带的 -D,因此在其 CMakeLists 被处理前把它换成伪栈模式:
# 临时内存改由 opus_alloc_scratch() 从堆分配。代价是编码不可多线程 —— 本应用只有
# 一个编码任务,无影响。managed_components 每次构建都会重新生成,替换可重复。
set(_sb_opus_dir "${CMAKE_CURRENT_LIST_DIR}/managed_components/78__esp-opus")
if(EXISTS "${_sb_opus_dir}/CMakeLists.txt")
    file(READ "${_sb_opus_dir}/CMakeLists.txt" _sb_opus_cmake)
    string(REPLACE "-DUSE_ALLOCA" "-DNONTHREADSAFE_PSEUDOSTACK"
           _sb_opus_cmake "${_sb_opus_cmake}")
    file(WRITE "${_sb_opus_dir}/CMakeLists.txt" "${_sb_opus_cmake}")
    message(STATUS "stackbear: libopus scratch -> NONTHREADSAFE_PSEUDOSTACK")
else()
    message(FATAL_ERROR "stackbear: ${_sb_opus_dir}/CMakeLists.txt 不存在;组件解析时机可能已变")
endif()
