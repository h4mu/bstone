LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := main

SDL_PATH := ../SDL

LOCAL_C_INCLUDES += $(LOCAL_PATH)/$(SDL_PATH)/include $(LOCAL_PATH)/../../../dosbox

# Add your application source files here...
LOCAL_SRC_FILES := $(SDL_PATH)/src/main/android/SDL_android_main.c \
../../../3d_act1.cpp ../../../3d_act2.cpp ../../../3d_agent.cpp ../../../3d_debug.cpp ../../../3d_draw2.cpp ../../../3d_draw.cpp ../../../3d_game.cpp ../../../3d_inter.cpp ../../../3d_main.cpp ../../../3d_menu.cpp ../../../3d_msgs.cpp ../../../3d_play.cpp ../../../3d_scale.cpp ../../../3d_state.cpp ../../../bstone_adlib_decoder.cpp ../../../bstone_adlib_music_decoder.cpp ../../../bstone_adlib_sfx_decoder.cpp ../../../bstone_audio_decoder.cpp ../../../bstone_audio_mixer.cpp ../../../bstone_binary_reader.cpp ../../../bstone_binary_writer.cpp ../../../bstone_cl_args.cpp ../../../bstone_crc32.cpp ../../../bstone_file_stream.cpp ../../../bstone_format_string.cpp ../../../bstone_istream.cpp ../../../bstone_log.cpp ../../../bstone_memory_binary_reader.cpp ../../../bstone_memory_stream.cpp ../../../bstone_ogl_api.cpp ../../../bstone_ogl_version.cpp ../../../bstone_opl2.cpp ../../../bstone_pcm_decoder.cpp ../../../bstone_precompiled.cpp ../../../bstone_string_helper.cpp ../../../colormap.cpp ../../../d3_d2.cpp ../../../d3_dr2.cpp ../../../id_ca.cpp ../../../id_in.cpp ../../../id_pm.cpp ../../../id_sd.cpp ../../../id_sdd.cpp ../../../id_us_1.cpp ../../../id_us.cpp ../../../id_vh.cpp ../../../id_vl.cpp ../../../jm_cio.cpp ../../../jm_free.cpp ../../../jm_io.cpp ../../../jm_lzh.cpp ../../../jm_tp.cpp ../../../markhack.cpp ../../../movie.cpp ../../../scale.cpp ../../../stub.cpp ../../../vgapal.cpp ../../../dosbox/dbopl.cpp

LOCAL_SHARED_LIBRARIES += SDL2

LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -llog -latomic

include $(BUILD_SHARED_LIBRARY)
