/*
BStone: A Source port of
Blake Stone: Aliens of Gold and Blake Stone: Planet Strike

Copyright (c) 1992-2013 Apogee Entertainment, LLC
Copyright (c) 2013-2015 Boris I. Bendovsky (bibendovsky@hotmail.com)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the
Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/


#include <stdexcept>
#include <vector>
#include "id_heads.h"
#include "bstone_ogl_api.h"

#if defined(BSTONE_PANDORA) // Pandora VSync Support
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif
static int fbdev = -1;
#endif // BSTONE_PANDORA


int bufferofs;
int displayofs;

int* ylookup = nullptr;

bool screenfaded;

uint8_t palette1[256][3];
uint8_t palette2[256][3];


bool is_aog_full();
bool is_aog_sw();
bool is_ps();


// BBi
namespace {


//
// Common stuff
//

SDL_DisplayMode display_mode;

const int default_window_width = 640;
const int default_window_height = 480;

int window_width = 0;
int window_height = 0;


uint8_t* vga_palette = nullptr;

bool (* vid_pre_subsystem_creation)() = nullptr;
bool (* vid_pre_window_creation)() = nullptr;
uint32_t (* vid_get_window_flags)() = nullptr;
bool (* vid_initialize_renderer)() = nullptr;
void (* vid_draw_screen)() = nullptr;
bool (* vid_update_viewport)() = nullptr;
void (* vid_refresh_screen)() = nullptr;
void (* vid_update_screen)() = nullptr;
void (* vid_uninitialize_video)() = nullptr;

bool sdl_use_custom_window_position = false;
int sdl_window_x = 0;
int sdl_window_y = 0;

void initialize_video();
void uninitialize_video();


//
// OpenGL stuff
//

enum class OglObjectType {
    none,
    shader,
    program,
}; // OglObjectType


struct ScreenVertex {
    float x;
    float y;

    float s;
    float t;
}; // ScreenVertex


void ogl_draw_screen();
void ogl_refresh_screen();
void ogl_update_screen();

void ogl_uninitialize_video();


static const GLchar* screen_fs_text =
#ifdef BSTONE_USE_GLES
    "#version 100\n"
    "precision mediump float;\n"
#else
    "#version 120\n"
#endif

    "uniform sampler2D screen_tu;\n"
    "uniform sampler2D palette_tu;\n"

    "varying vec2 tc;\n"

    "void main()\n"
    "{\n"
    "    vec2 palette_index = texture2D(screen_tu, tc).rg;\n"
    "    palette_index = clamp(palette_index, 0.0, 1.0);\n"
    "    vec4 color = vec4(texture2D(palette_tu, palette_index).rgb, 1.0);\n"
    "    gl_FragColor = (color * 255.0) / 63.0;\n"
    "}\n"
;

static const GLchar* screen_vs_text =
#ifdef BSTONE_USE_GLES
    "#version 100\n"
    "precision mediump float;\n"
#else
    "#version 120\n"
#endif

    "attribute vec4 pos_vec4;\n"
    "attribute vec2 tc0_vec2;\n"

    "uniform mat4 proj_mat4;\n"

    "varying vec2 tc;\n"

    "void main()\n"
    "{\n"
    "    tc = tc0_vec2;\n"
    "    gl_Position = proj_mat4 * pos_vec4;\n"
    "}\n"
;


GLuint screen_tex = GL_NONE;
GLuint palette_tex = GL_NONE;
GLuint screen_vbo = GL_NONE;
GLuint screen_fso = GL_NONE;
GLuint screen_vso = GL_NONE;
GLuint screen_po = GL_NONE;


ScreenVertex screen_vertices[4];

// vertex attribute: position
GLint a_pos_vec4 = -1;

// vertex attribute: texture coordinates
GLint a_tc0_vec2 = -1;

// uniform: projection matrix
GLint u_proj_mat4 = -1;

// uniform: screen texture unit
GLint u_screen_tu = -1;

// uniform: palette texture unit
GLint u_palette_tu = -1;


SDL_GLContext sdl_ogl_context = nullptr;


//
// Software stuff
//

const uint32_t sdl_pixel_format = SDL_PIXELFORMAT_RGBA8888;


class SdlPalette {
public:
    SdlPalette() :
        palette_(),
        color_shifts_()
    {
    }

    ~SdlPalette()
    {
    }

    uint32_t operator[](
        int index) const
    {
        if (!is_initialized()) {
            throw std::runtime_error("Not initialized.");
        }

        return palette_[index];
    }

    bool initialize(
        uint32_t pixel_format)
    {
        uninitialize();

        bstone::Log::write(
            "SDL: Initializing SDL palette...");

        using Masks = std::vector<uint32_t>;

        int bpp = 0;
        Masks masks(4);

        SDL_bool sdl_result = SDL_FALSE;

        sdl_result = ::SDL_PixelFormatEnumToMasks(
            pixel_format,
            &bpp,
            &masks[0],
            &masks[1],
            &masks[2],
            &masks[3]);

        if (sdl_result == SDL_FALSE) {
            bstone::Log::write_error("SDL: {}", SDL_GetError());
            return false;
        }

        if (bpp != 32) {
            bstone::Log::write_error(
                "SDL: Pixel format should have 32 bits per pixel.");
            return false;
        }

        Palette palette(256);

        ColorShifts color_shifts(4);

        for (int i = 0; i < 4; ++i) {
            color_shifts[i] = get_color_shift(masks[i]);
        }

        palette_.swap(palette);
        color_shifts_.swap(color_shifts);

        return true;
    }

    void uninitialize()
    {
        Palette().swap(palette_);
        ColorShifts().swap(color_shifts_);
    }

    void update(
        const uint8_t* palette,
        int offset,
        int count)
    {
        if (!is_initialized()) {
            throw std::runtime_error("Not initialized.");
        }

        if (offset < 0 || offset > 256) {
            throw std::out_of_range("Invalid offset.");
        }

        if (count < 0 || count > 256) {
            throw std::out_of_range("Invalid count.");
        }

        if ((offset + count) > 256) {
            throw std::out_of_range("Invalid offset or count.");
        }

        for (int i = 0; i < count; ++i) {
            const auto palette_color = &palette[3 * i];

            uint32_t color = 0;

            for (int j = 0; j < 3; ++j) {
                auto vga_color = static_cast<uint32_t>(palette_color[j]);

                uint32_t pc_color = (255 * vga_color) / 63;

                color |= pc_color << color_shifts_[j];
            }

            color |= 0x000000FF << color_shifts_[3];

            palette_[offset + i] = color;
        }
    }

    bool is_initialized() const
    {
        return !palette_.empty();
    }


private:
    using Palette = std::vector<uint32_t>;
    using ColorShifts = std::vector<int>;

    Palette palette_;
    ColorShifts color_shifts_;

    SdlPalette(
        const SdlPalette& that);

    SdlPalette& operator=(
        const SdlPalette& that);

    static int get_color_shift(
        uint32_t mask)
    {
        switch (mask) {
        case 0x000000FF:
            return 0;

        case 0x0000FF00:
            return 8;

        case 0x00FF0000:
            return 16;

        case 0xFF000000:
            return 24;

        default:
            throw std::invalid_argument("Invalid mask.");
        }
    }
}; // SdlPalette


SDL_Renderer* sdl_soft_renderer = nullptr;
SDL_Texture* sdl_soft_screen_tex = nullptr;
SdlPalette sdl_palette;


void soft_draw_screen();
void soft_refresh_screen();
void soft_update_screen();

void soft_uninitialize_video();


} // namespace


enum RendererType {
    RT_NONE,
    RT_AUTO_DETECT,
    RT_SOFTWARE,
    RT_OPEN_GL
}; // RendererType


extern const uint8_t vgapal[768];

uint8_t* vga_memory = nullptr;

int vga_scale = 0;
int vga_width = 0;
int vga_height = 0;
int vga_area = 0;

int screen_x = 0;
int screen_y = 0;

int screen_width = 0;
int screen_height = 0;

bool sdl_is_windowed = false;
SDL_Window* sdl_window = nullptr;
RendererType g_renderer_type;
bool vid_has_vsync = false;
bool vid_stretch = default_vid_stretch;


void check_vsync()
{
    const int draw_count = 10;

    const int duration_tolerance_pct = 25;

    const int expected_duration_ms =
        (1000 * draw_count) / ::display_mode.refresh_rate;

    const int min_expected_duration_ms =
        ((100 - duration_tolerance_pct) * expected_duration_ms) / 100;

    auto before_timestamp = Clock::now();

    for (int i = 0; i < draw_count; ++i) {
        ::vid_draw_screen();
    }

    auto after_timestamp = Clock::now();

    auto duration = after_timestamp - before_timestamp;

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        duration).count();

    ::vid_has_vsync = (duration_ms >= min_expected_duration_ms);
}
// BBi


// ===========================================================================

// asm

void VL_WaitVBL(
    uint32_t vbls)
{
    if (vbls == 0) {
        return;
    }

    if (vbls > 1) {
        ::sys_sleep_for(1000 * vbls / TickBase);
        return;
    }

    if (::vid_has_vsync) {
        return;
    }


    static const uint32_t one_tick_delay = 1000 / TickBase;

    auto timer_ticks = ::sys_get_timer_ticks();
    auto current_ticks = ::SDL_GetTicks();

    if (current_ticks > timer_ticks) {
        uint32_t diff = current_ticks - timer_ticks;

        if (one_tick_delay >= diff) {
            uint32_t remain = one_tick_delay - diff;
            ::sys_sleep_for(remain);
        }
    }
}

// ===========================================================================


// BBi Moved from jm_free.cpp
void VL_Startup()
{
}
// BBi

void VL_Shutdown()
{
    uninitialize_video();
}

void VL_SetVGAPlaneMode()
{
    ::initialize_video();

    const int vga_size = ::vga_scale * ::vga_scale * ::vga_ref_size;

    delete [] ::vga_memory;
    ::vga_memory = new uint8_t[vga_size];
    std::uninitialized_fill_n(::vga_memory, vga_size, 0);

    delete [] ::vga_palette;
    ::vga_palette = new uint8_t[::vga_palette_size];
    std::uninitialized_fill_n(::vga_palette, ::vga_palette_size, 0);

    ::vid_refresh_screen();
    ::in_handle_events();

    ::check_vsync();
}

// ===========================================================================

/*
====================
=
= VL_SetLineWidth
=
= Line witdh is in WORDS, 40 words is normal width for vgaplanegr
=
====================
*/

void VL_SetLineWidth(
    int width)
{
    static_cast<void>(width);

    delete [] ::ylookup;
    ::ylookup = new int[::vga_height];

    for (int i = 0; i < ::vga_height; ++i) {
        ::ylookup[i] = i * ::vga_width;
    }
}


/*
=============================================================================

                                PALETTE OPS

                To avoid snow, do a WaitVBL BEFORE calling these

=============================================================================
*/

void VL_FillPalette(
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
    for (int i = 0; i < 256; ++i) {
        ::vga_palette[(3 * i) + 0] = red;
        ::vga_palette[(3 * i) + 1] = green;
        ::vga_palette[(3 * i) + 2] = blue;
    }

    ::VL_SetPalette(0, 255, ::vga_palette);
}

void VL_SetPalette(
    int first,
    int count,
    const uint8_t* palette,
    bool refresh_screen)
{
    int offset = 3 * first;
    int size = 3 * count;

    std::uninitialized_copy_n(
        palette,
        size,
        &::vga_palette[offset]);

    switch (g_renderer_type) {
    case RT_OPEN_GL:
        ::glActiveTexture(GL_TEXTURE1);

        ::glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            256,
            1,
            GL_RGB,
            GL_UNSIGNED_BYTE,
            ::vga_palette);

        if (refresh_screen) {
            ::ogl_refresh_screen();
        }
        break;

    case RT_SOFTWARE:
        ::sdl_palette.update(palette, first, count);

        if (refresh_screen) {
            ::soft_refresh_screen();
        }
        break;

    default:
        throw std::runtime_error("g_renderer_type");
    }
}

void VL_GetPalette(
    int first,
    int count,
    uint8_t* palette)
{
    int offset = 3 * first;
    int size = 3 * count;

    std::uninitialized_copy_n(
        &::vga_palette[offset],
        size,
        palette);
}

/*
=================
=
= VL_FadeOut
=
= Fades the current palette to the given color in the given number of steps
=
=================
*/

void VL_FadeOut(
    int start,
    int end,
    int red,
    int green,
    int blue,
    int steps)
{
    int orig;
    int delta;

    ::VL_GetPalette(0, 256, &::palette1[0][0]);

    std::uninitialized_copy_n(
        &::palette1[0][0],
        768,
        &::palette2[0][0]);

    //
    // fade through intermediate frames
    //
    for (int i = 0; i < steps; ++i) {
        auto origptr = &::palette1[start][0];
        auto newptr = &::palette2[start][0];

        for (int j = start; j <= end; ++j) {
            orig = *origptr++;
            delta = red - orig;
            *newptr++ = static_cast<uint8_t>(orig + ((delta * i) / steps));
            orig = *origptr++;
            delta = green - orig;
            *newptr++ = static_cast<uint8_t>(orig + ((delta * i) / steps));
            orig = *origptr++;
            delta = blue - orig;
            *newptr++ = static_cast<uint8_t>(orig + ((delta * i) / steps));
        }

        ::VL_SetPalette(0, 256, &::palette2[0][0]);
        ::VL_WaitVBL(1);
    }

    //
    // final color
    //
    ::VL_FillPalette(
        static_cast<uint8_t>(red),
        static_cast<uint8_t>(green),
        static_cast<uint8_t>(blue));

    ::VL_WaitVBL(1);

    screenfaded = true;
}

void VL_FadeIn(
    int start,
    int end,
    const uint8_t* palette,
    int steps)
{
    ::VL_GetPalette(0, 256, &::palette1[0][0]);

    std::uninitialized_copy_n(
        &::palette1[0][0],
        768,
        &::palette2[0][0]);

    start *= 3;
    end = (end * 3) + 2;

    //
    // fade through intermediate frames
    //
    for (int i = 0; i < steps; ++i) {
        for (int j = start; j <= end; ++j) {
            int delta = palette[j] - ::palette1[0][j];

            ::palette2[0][j] =
                static_cast<uint8_t>(::palette1[0][j] + ((delta * i) / steps));
        }

        ::VL_SetPalette(0, 256, &::palette2[0][0]);
        ::VL_WaitVBL(1);
    }

    //
    // final color
    //
    ::VL_SetPalette(0, 256, palette);
    ::VL_WaitVBL(1);

    screenfaded = false;
}

void VL_SetPaletteIntensity(
    int start,
    int end,
    const uint8_t* palette,
    int intensity)
{
    auto cmap = &::palette1[0][0] + (start * 3);

    intensity = 63 - intensity;

    for (int loop = start; loop <= end; ++loop) {
        int red = (*palette++) - intensity;

        if (red < 0) {
            red = 0;
        }

        *cmap++ = static_cast<uint8_t>(red);

        int green = *palette++ - intensity;

        if (green < 0) {
            green = 0;
        }

        *cmap++ = static_cast<uint8_t>(green);

        int blue = *palette++ - intensity;

        if (blue < 0) {
            blue = 0;
        }

        *cmap++ = static_cast<uint8_t>(blue);
    }

    ::VL_SetPalette(start, end - start + 1, &::palette1[0][0]);
}

/*
=============================================================================

 PIXEL OPS

=============================================================================
*/

void VL_Plot(
    int x,
    int y,
    uint8_t color)
{
    int offset = ::vl_get_offset(::bufferofs, x, y);

    for (int i = 0; i < vga_scale; ++i) {
        std::uninitialized_fill_n(
            &::vga_memory[offset],
            ::vga_scale,
            color);

        offset += ::vga_width;
    }
}

void VL_Hlin(
    int x,
    int y,
    int width,
    uint8_t color)
{
    ::VL_Bar(x, y, width, 1, color);
}

void VL_Vlin(
    int x,
    int y,
    int height,
    uint8_t color)
{
    ::VL_Bar(x, y, 1, height, color);
}

void VL_Bar(
    int x,
    int y,
    int width,
    int height,
    uint8_t color)
{
    width *= ::vga_scale;
    height *= ::vga_scale;

    int offset = ::vl_get_offset(::bufferofs, x, y);

    if (x == 0 && width == ::vga_width) {
        int count = height * ::vga_width;
        std::uninitialized_fill_n(&::vga_memory[offset], count, color);
    } else {
        for (int i = 0; i < height; ++i) {
            std::uninitialized_fill_n(&::vga_memory[offset], width, color);
            offset += vga_width;
        }
    }
}

/*
============================================================================

 MEMORY OPS

============================================================================
*/

void VL_MemToLatch(
    const uint8_t* source,
    int width,
    int height,
    int dest)
{
    int dst_pitch = ::vga_scale * width;
    int base_offset = ::vl_get_offset(dest);

    for (int p = 0; p < 4; ++p) {
        for (int h = 0; h < height; ++h) {
            for (int w = p; w < width; w += 4) {
                uint8_t pixel = *source++;

                int offset = base_offset +
                    ::vga_scale * ((::vga_scale * h * width) + w);

                for (int s = 0; s < ::vga_scale; ++s) {
                    std::uninitialized_fill_n(
                        &::vga_memory[offset],
                        ::vga_scale,
                        pixel);

                    offset += dst_pitch;
                }
            }
        }
    }
}

void VL_MemToScreen(
    const uint8_t* source,
    int width,
    int height,
    int x,
    int y)
{
    for (int p = 0; p < 4; ++p) {
        for (int h = 0; h < height; ++h) {
            for (int w = p; w < width; w += 4) {
                ::VL_Plot(x + w, y + h, *source++);
            }
        }
    }
}

void VL_MaskMemToScreen(
    const uint8_t* source,
    int width,
    int height,
    int x,
    int y,
    uint8_t mask)
{
    for (int p = 0; p < 4; ++p) {
        for (int h = 0; h < height; ++h) {
            for (int w = p; w < width; w += 4) {
                uint8_t color = *source++;

                if (color != mask) {
                    ::VL_Plot(x + w, y + h, color);
                }
            }
        }
    }
}

void VL_ScreenToMem(
    uint8_t* dest,
    int width,
    int height,
    int x,
    int y)
{
    for (int p = 0; p < 4; ++p) {
        for (int h = 0; h < height; ++h) {
            for (int w = p; w < width; w += 4) {
                *dest++ = ::vl_get_pixel(::bufferofs, x + w, y + h);
            }
        }
    }
}

void VL_LatchToScreen(
    int source,
    int width,
    int height,
    int x,
    int y)
{
    int src_pitch = ::vga_scale * 4 * width;
    int src_offset = ::vl_get_offset(source);
    int dst_offset = ::vl_get_offset(::bufferofs, x, y);

    for (int h = 0; h < height; ++h) {
        for (int s = 0; s < ::vga_scale; ++s) {
            std::uninitialized_copy_n(
                &::vga_memory[src_offset],
                src_pitch,
                &::vga_memory[dst_offset]);

            src_offset += src_pitch;
            dst_offset += vga_width;
        }
    }
}

void VL_ScreenToScreen(
    int source,
    int dest,
    int width,
    int height)
{
    source *= 4 * ::vga_scale * ::vga_scale;
    dest *= 4 * ::vga_scale * ::vga_scale;
    width *= 4 * ::vga_scale;
    height *= ::vga_scale;

    auto src_pixels = &::vga_memory[source];
    auto dst_pixels = &::vga_memory[dest];

    for (int h = 0; h < height; ++h) {
        std::uninitialized_copy_n(
            src_pixels,
            width,
            dst_pixels);

        src_pixels += vga_width;
        dst_pixels += vga_width;
    }
}


// BBi
namespace {


// Builds an orthographic projection matrix with upside-down origin.
void ogl_ortho(
    int width,
    int height,
    float matrix[16])
{
    assert(width > 0);
    assert(height > 0);
    assert(matrix);

    matrix[0] = 2.0F / width;
    matrix[1] = 0.0F;
    matrix[2] = 0.0F;
    matrix[3] = 0.0F;

    matrix[4] = 0.0F;
    matrix[5] = -2.0F / height;
    matrix[6] = 0.0F;
    matrix[7] = 0.0F;

    matrix[8] = 0.0F;
    matrix[9] = 0.0F;
    matrix[10] = -1.0F;
    matrix[11] = 0.0F;

    matrix[12] = -1.0F;
    matrix[13] = 1.0F;
    matrix[14] = 0.0F;
    matrix[15] = 1.0F;
}

// BBi For debugging puprpose
#if 0
// Clears error flags and returns true if any error flag was set,
// otherwise returns false.
bool ogl_check_for_and_clear_errors()
{
    bool result = false;

    while (::glGetError() != GL_NONE) {
        result = true;
    }

    return result;
}
#endif

// Just draws a screen texture.
void ogl_draw_screen()
{
    ::glClear(GL_COLOR_BUFFER_BIT);
    ::glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

#if defined(BSTONE_PANDORA) // Pandora VSync
    if (::fbdev >= 0) {
        int arg = 0;
        ::ioctl(fbdev, FBIO_WAITFORVSYNC, &arg);
    }
#endif

    ::SDL_GL_SwapWindow(::sdl_window);
}

bool ogl_update_viewport()
{
    bstone::Log::write("OGL: Updating a view port...");

    ::glClear(GL_COLOR_BUFFER_BIT);

    if (::vid_stretch) {
        ::glViewport(0, 0, ::window_width, ::window_height);
    } else {
        ::glViewport(::screen_x, ::screen_y, ::screen_width, ::screen_height);
    }

    return true;
}

// Updates screen texture with display data and
// draws it.
void ogl_refresh_screen()
{
    GLenum format =
        bstone::OglApi::has_ext_texture_rg() ?
        bstone::OglApi::get_gl_red() :
        GL_LUMINANCE;

    ::glActiveTexture(GL_TEXTURE0);

    ::glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        ::vga_width,
        ::vga_height,
        format,
        GL_UNSIGNED_BYTE,
        &::vga_memory[::vl_get_offset(::displayofs)]);

    ::ogl_draw_screen();
}

// Copies buffer page to a display one,
// updates screen texture with display page data
// and draws it.
void ogl_update_screen()
{
    if (::displayofs != ::bufferofs) {
        int src_offset = ::vl_get_offset(::bufferofs);
        int dst_offset = ::vl_get_offset(::displayofs);

        std::uninitialized_copy_n(
            &::vga_memory[src_offset],
            ::vga_area,
            &::vga_memory[dst_offset]);
    }

    ::ogl_refresh_screen();
}

// Returns an information log of a shader or a program.
std::string ogl_get_info_log(
    GLuint object)
{
    if (object == GL_NONE) {
        return std::string();
    }

    auto object_type = OglObjectType::none;
    GLint info_log_size = 0; // with a null terminator

    if (::glIsShader(object)) {
        object_type = OglObjectType::shader;

        ::glGetShaderiv(
            object,
            GL_INFO_LOG_LENGTH,
            &info_log_size);
    } else if (::glIsProgram(object)) {
        object_type = OglObjectType::program;

        ::glGetProgramiv(
            object,
            GL_INFO_LOG_LENGTH,
            &info_log_size);
    } else {
        return std::string();
    }

    if (info_log_size <= 1) {
        return std::string();
    }

    GLsizei info_log_length; // without a null terminator
    std::vector<GLchar> info_log(info_log_size, '\0');

    switch (object_type) {
    case OglObjectType::shader:
        ::glGetShaderInfoLog(
            object,
            info_log_size,
            &info_log_length,
            info_log.data());
        break;

    case OglObjectType::program:
        ::glGetProgramInfoLog(
            object,
            info_log_size,
            &info_log_length,
            info_log.data());
        break;

    default:
        return std::string();
    }

    if (info_log_length > 0) {
        return info_log.data();
    }

    return std::string();
}

bool ogl_load_shader(
    GLuint shader_object,
    const GLchar* shader_text)
{
    GLint compile_status = GL_FALSE;
    const GLchar* lines[1] = { shader_text };
    GLint lengths[1] = {
        static_cast<GLint>(std::string::traits_type::length(shader_text))
    };

    ::glShaderSource(shader_object, 1, lines, lengths);
    ::glCompileShader(shader_object);
    ::glGetShaderiv(shader_object, GL_COMPILE_STATUS, &compile_status);

    auto shader_log = ::ogl_get_info_log(shader_object);

    if (compile_status != GL_FALSE) {
        if (!shader_log.empty()) {
            bstone::Log::write_warning(shader_log);
        }

        return true;
    }

    if (shader_log.empty()) {
        shader_log = "Generic compile error.";
    }

    bstone::Log::write_error(shader_log);

    return false;
}

bool ogl_initialize_textures()
{
    bstone::Log::write("OGL: Initializing textures...");

    bool is_succeed = true;

    if (is_succeed) {
        ::screen_tex = GL_NONE;
        ::glGenTextures(1, &::screen_tex);

        if (::screen_tex == GL_NONE) {
            is_succeed = false;
            bstone::Log::write_error("Failed to generate a screen texture.");
        }
    }

    GLenum format = GL_NONE;
    GLenum internal_format = GL_NONE;

    if (is_succeed) {
        if (bstone::OglApi::has_ext_texture_rg()) {
            format = bstone::OglApi::get_gl_red();
            internal_format = bstone::OglApi::get_gl_r8();
        } else {
            format = GL_LUMINANCE;
            internal_format = GL_LUMINANCE;
        }

        ::glActiveTexture(GL_TEXTURE0);
        ::glBindTexture(GL_TEXTURE_2D, ::screen_tex);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        ::glTexImage2D(
            GL_TEXTURE_2D,
            0,
            internal_format,
            ::vga_width,
            ::vga_height,
            0,
            format,
            GL_UNSIGNED_BYTE,
            nullptr);
    }

    if (is_succeed) {
        ::palette_tex = GL_NONE;
        ::glGenTextures(1, &::palette_tex);

        if (::palette_tex == GL_NONE) {
            is_succeed = false;
            bstone::Log::write_error("Failed to generate a palette texture.");
        }
    }

    if (is_succeed) {
        ::glActiveTexture(GL_TEXTURE1);
        ::glBindTexture(GL_TEXTURE_2D, ::palette_tex);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        ::glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGB,
            256,
            1,
            0,
            GL_RGB,
            GL_UNSIGNED_BYTE,
            nullptr);
    }

    if (is_succeed) {
        return true;
    }

    if (::screen_tex != GL_NONE) {
        ::glDeleteTextures(1, &::screen_tex);
        ::screen_tex = GL_NONE;
    }

    if (::palette_tex != GL_NONE) {
        ::glDeleteTextures(1, &::palette_tex);
        ::palette_tex = GL_NONE;
    }

    return false;
}

bool ogl_initialize_vertex_buffers()
{
    bstone::Log::write(
        "OGL: Setting up a screen buffer object...");


    ::screen_vbo = GL_NONE;
    ::glGenBuffers(1, &::screen_vbo);

    if (::screen_vbo == GL_NONE) {
        bstone::Log::write_error("Failed to create an object.");
        return false;
    }


    ScreenVertex* vertex = nullptr;

    vertex = &::screen_vertices[0];
    vertex->x = 0.0F;
    vertex->y = 0.0F;
    vertex->s = 0.0F;
    vertex->t = 0.0F;

    vertex = &::screen_vertices[1];
    vertex->x = 0.0F;
    vertex->y = static_cast<float>(::vga_height);
    vertex->s = 0.0F;
    vertex->t = 1.0F;

    vertex = &::screen_vertices[2];
    vertex->x = static_cast<float>(::vga_width);
    vertex->y = 0.0F;
    vertex->s = 1.0F;
    vertex->t = 0.0F;

    vertex = &::screen_vertices[3];
    vertex->x = static_cast<float>(::vga_width);
    vertex->y = static_cast<float>(::vga_height);
    vertex->s = 1.0F;
    vertex->t = 1.0F;

    ::glBindBuffer(GL_ARRAY_BUFFER, ::screen_vbo);

    ::glBufferData(
        GL_ARRAY_BUFFER,
        sizeof(::screen_vertices),
        ::screen_vertices,
        GL_STATIC_DRAW);

    return true;
}

bool ogl_initialize_shaders()
{
    bool is_succeed = true;

    if (is_succeed) {
        ::screen_fso = ::glCreateShader(GL_FRAGMENT_SHADER);

        if (::screen_fso == GL_NONE) {
            is_succeed = false;
            bstone::Log::write_error("Failed to create an object.");
        }
    }

    if (is_succeed) {
        bstone::Log::write("OGL: Loading a screen fragment shader...");

        is_succeed = ::ogl_load_shader(::screen_fso, ::screen_fs_text);
    }

    if (is_succeed) {
        ::screen_vso = ::glCreateShader(GL_VERTEX_SHADER);

        if (::screen_vso == GL_NONE) {
            is_succeed = false;
            bstone::Log::write_error("Failed to create an object.");
        }
    }

    if (is_succeed) {
        bstone::Log::write("OGL: Loading a screen vertex shader...");

        is_succeed = ::ogl_load_shader(::screen_vso, ::screen_vs_text);
    }

    return is_succeed;
}

bool ogl_initialize_programs()
{
    bstone::Log::write("OGL: Setting up a screen program object...");

    bool is_succeed = true;

    if (is_succeed) {
        ::screen_po = ::glCreateProgram();

        if (::screen_po == GL_NONE) {
            is_succeed = false;

            bstone::Log::write_error("Failed to create an object.");
        }
    }

    if (is_succeed) {
        GLint link_status = GL_FALSE;

        ::glAttachShader(::screen_po, ::screen_fso);
        ::glAttachShader(::screen_po, ::screen_vso);
        ::glLinkProgram(::screen_po);
        ::glGetProgramiv(::screen_po, GL_LINK_STATUS, &link_status);

        auto program_log = ogl_get_info_log(screen_po);

        if (link_status != GL_FALSE) {
            if (!program_log.empty()) {
                bstone::Log::write_warning(program_log);
            }
        } else {
            is_succeed = false;

            if (program_log.empty()) {
                program_log = "Generic link error.";
            }

            bstone::Log::write_error(program_log);
        }
    }

    if (is_succeed) {
        ::glUseProgram(::screen_po);

        a_pos_vec4 = ::glGetAttribLocation(::screen_po, "pos_vec4");
        a_tc0_vec2 = ::glGetAttribLocation(::screen_po, "tc0_vec2");

        float proj_mat4[16];
        ::u_proj_mat4 = ::glGetUniformLocation(::screen_po, "proj_mat4");
        ::ogl_ortho(::vga_width, ::vga_height, proj_mat4);
        ::glUniformMatrix4fv(::u_proj_mat4, 1, GL_FALSE, proj_mat4);

        ::u_screen_tu = ::glGetUniformLocation(::screen_po, "screen_tu");
        ::glUniform1i(::u_screen_tu, 0);

        ::u_palette_tu = ::glGetUniformLocation(::screen_po, "palette_tu");
        ::glUniform1i(::u_palette_tu, 1);

        bstone::Log::write("OGL: Done.");
    }

    return is_succeed;
}

void ogl_uninitialize_video()
{
    if (::sdl_ogl_context) {
        if (::screen_po != GL_NONE) {
            ::glDisableVertexAttribArray(::a_pos_vec4);
            ::glDisableVertexAttribArray(::a_tc0_vec2);

            ::glUseProgram(GL_NONE);
            ::glDetachShader(::screen_po, ::screen_fso);
            ::glDetachShader(::screen_po, ::screen_vso);
            ::glDeleteProgram(::screen_po);
            ::screen_po = GL_NONE;
        }

        if (::screen_fso != GL_NONE) {
            ::glDeleteShader(::screen_fso);
            ::screen_fso = GL_NONE;
        }

        if (::screen_vso != GL_NONE) {
            ::glDeleteShader(::screen_vso);
            ::screen_vso = GL_NONE;
        }

        if (::screen_vbo != GL_NONE) {
            ::glDeleteBuffers(1, &::screen_vbo);
            ::screen_vbo = GL_NONE;
        }

        if (::screen_tex != GL_NONE) {
            ::glDeleteTextures(1, &::screen_tex);
            ::screen_tex = GL_NONE;
        }

        if (::palette_tex != GL_NONE) {
            ::glDeleteTextures(1, &::palette_tex);
            ::palette_tex = GL_NONE;
        }

        ::SDL_GL_MakeCurrent(::sdl_window, nullptr);
        ::SDL_GL_DeleteContext(::sdl_ogl_context);
        ::sdl_ogl_context = nullptr;

        bstone::OglApi::uninitialize();
    }

    ::a_pos_vec4 = -1;
    ::a_tc0_vec2 = -1;
    ::u_screen_tu = -1;
    ::u_palette_tu = -1;

#if defined(BSTONE_PANDORA) // Pandora VSync
    ::close(::fbdev);
    ::fbdev = -1;
#endif
}

bool ogl_pre_subsystem_creation()
{
#if defined(BSTONE_PANDORA) // Pandora VSync
    ::fbdev = ::open("/dev/fb0", O_RDONLY /* O_RDWR */);
    if (::fbdev < 0) {
        bstone::Log::write_warning(
            "SDL: Couldn't open /dev/fb0 for Pandora VSync...");
    }
#endif

    return true;
}

bool ogl_pre_window_creation()
{
    int errors = 0;

    ::SDL_GL_ResetAttributes();

#if defined(BSTONE_USE_GLES)
    errors += ::SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    errors += ::SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
    errors += ::SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    errors += ::SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    errors += ::SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, 16);
    errors += ::SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    errors += ::SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_ES);
    errors &= ::SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
#else
    errors += ::SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    errors += ::SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    errors += ::SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    errors += ::SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    errors += ::SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, 32);
    errors += ::SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#endif

    errors += SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    errors += SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    if (errors != 0) {
        bstone::Log::write_error("SDL: Failed to set OpenGL attributes.");
        bstone::Log::write_error("SDL: {}", ::SDL_GetError());
    }

    return errors == 0;
}

uint32_t ogl_get_window_flags()
{
    return SDL_WINDOW_OPENGL;
}

bool ogl_initialize_renderer()
{
    bool is_succeed = true;

    if (is_succeed) {
        bstone::Log::write("SDL: Creating an OpenGL context...");

        ::sdl_ogl_context = ::SDL_GL_CreateContext(::sdl_window);

        if (!::sdl_ogl_context) {
            is_succeed = false;
            bstone::Log::write_error(::SDL_GetError());
        }
    }

    if (is_succeed) {
        is_succeed = bstone::OglApi::initialize();
    }

    if (is_succeed) {
        bstone::Log::write(
            "OGLAPI: {}: {}",
            "Vendor",
            bstone::OglApi::get_vendor());

        bstone::Log::write(
            "OGLAPI: {}: {}",
            "Renderer",
            bstone::OglApi::get_renderer());

        bstone::Log::write(
            "OGLAPI: {}: {}",
            "Original version",
            bstone::OglApi::get_version().get_original());

        bstone::Log::write(
            "OGLAPI: {}: {}",
            "Parsed version",
            bstone::OglApi::get_version().to_string());
    }

    if (is_succeed) {
        is_succeed = ::ogl_initialize_textures();
    }

    if (is_succeed) {
        is_succeed = ::ogl_initialize_vertex_buffers();
    }

    if (is_succeed) {
        is_succeed = ::ogl_initialize_shaders();
    }

    if (is_succeed) {
        is_succeed = ::ogl_initialize_programs();
    }

    if (is_succeed) {
        static_cast<void>(::ogl_update_viewport());

        ::glEnable(GL_TEXTURE_2D);

        ::glDisable(GL_CULL_FACE);
        ::glDisable(GL_DEPTH_TEST);

        if (::a_pos_vec4 != -1) {
            ::glVertexAttribPointer(
                ::a_pos_vec4,
                2,
                GL_FLOAT,
                GL_FALSE,
                sizeof(ScreenVertex),
                reinterpret_cast<const GLvoid*>(offsetof(ScreenVertex, x)));

            ::glEnableVertexAttribArray(::a_pos_vec4);
        }

        if (::a_tc0_vec2 != -1) {
            ::glVertexAttribPointer(
                ::a_tc0_vec2,
                2,
                GL_FLOAT,
                GL_FALSE,
                sizeof(ScreenVertex),
                reinterpret_cast<const GLvoid*>(offsetof(ScreenVertex, s)));

            ::glEnableVertexAttribArray(::a_tc0_vec2);
        }

        ::glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    }

    return is_succeed;
}

// Just draws a screen texture.
void soft_draw_screen()
{
    int sdl_result = 0;

    sdl_result = ::SDL_RenderCopy(
        ::sdl_soft_renderer,
        ::sdl_soft_screen_tex,
        nullptr,
        nullptr);

    if (sdl_result != 0) {
        bstone::Log::write_error("SOFT: Failed to copy data to renderer.");
    }

    ::SDL_RenderPresent(::sdl_soft_renderer);
}

bool soft_update_viewport()
{
    bstone::Log::write("SOFT: Updating a view port...");

    SDL_Rect view_port;
    view_port.x = (::vid_stretch ? 0 : ::screen_x);
    view_port.y = (::vid_stretch ? 0 : ::screen_y);
    view_port.w = (::vid_stretch ? ::window_width : ::screen_width);
    view_port.h = (::vid_stretch ? ::window_height : ::screen_height);

    bool is_succeed = true;
    int sdl_result = 0;

    if (is_succeed) {
        sdl_result = ::SDL_RenderClear(sdl_soft_renderer);

        if (sdl_result != 0) {
            bstone::Log::write_error(::SDL_GetError());
        }
    }

    if (is_succeed) {
        sdl_result = ::SDL_RenderSetViewport(
            ::sdl_soft_renderer,
            &view_port);

        if (sdl_result != 0) {
            bstone::Log::write_error(::SDL_GetError());
        }
    }

    return is_succeed;
}

// Updates screen texture with display data and
// draws it.
void soft_refresh_screen()
{
    SDL_Rect screen_rect;
    screen_rect.x = 0;
    screen_rect.y = 0;
    screen_rect.w = ::vga_width;
    screen_rect.h = ::vga_height;

    int sdl_result = 0;
    int pitch = 0;
    void* data = nullptr;

    sdl_result = ::SDL_LockTexture(
        ::sdl_soft_screen_tex,
        &screen_rect,
        &data,
        &pitch);

    static_cast<void>(sdl_result);

    auto octets = static_cast<uint8_t*>(data);
    int vga_offset = ::vl_get_offset(::displayofs);

    for (int y = 0; y < vga_height; ++y) {
        auto row = reinterpret_cast<uint32_t*>(octets);

        for (int x = 0; x < vga_width; ++x) {
            row[x] = ::sdl_palette[::vga_memory[vga_offset + x]];
        }

        octets += pitch;
        vga_offset += vga_width;
    }

    ::SDL_UnlockTexture(::sdl_soft_screen_tex);

    ::soft_draw_screen();
}

// Copies buffer page to a display one,
// updates screen texture with display page data
// and draws it.
void soft_update_screen()
{
    if (::displayofs != ::bufferofs) {
        int src_offset = ::vl_get_offset(::bufferofs);
        int dst_offset = ::vl_get_offset(::displayofs);

        std::uninitialized_copy_n(
            &::vga_memory[src_offset],
            ::vga_area,
            &::vga_memory[dst_offset]);
    }

    ::soft_refresh_screen();
}

bool soft_initialize_textures()
{
    bstone::Log::write("SDL: Creating a screen texture...");

    ::sdl_soft_screen_tex = ::SDL_CreateTexture(
        ::sdl_soft_renderer,
        ::sdl_pixel_format,
        SDL_TEXTUREACCESS_STREAMING,
        ::vga_width,
        ::vga_height);

    if (::sdl_soft_screen_tex) {
        return true;
    }

    bstone::Log::write_error(::SDL_GetError());

    return false;
}

void soft_uninitialize_textures()
{
    if (::sdl_soft_screen_tex) {
        ::SDL_DestroyTexture(::sdl_soft_screen_tex);
        ::sdl_soft_screen_tex = nullptr;
    }
}

void soft_uninitialize_video()
{
    ::soft_uninitialize_textures();

    if (::sdl_soft_renderer) {
        ::SDL_DestroyRenderer(::sdl_soft_renderer);
        ::sdl_soft_renderer = nullptr;
    }
}

bool soft_pre_subsystem_creation()
{
    return true;
}

bool soft_pre_window_creation()
{
    return ::sdl_palette.initialize(::sdl_pixel_format);
}

uint32_t soft_get_window_flags()
{
    return 0;
}

bool soft_initialize_renderer()
{
    bool is_succeed = true;

    if (is_succeed) {
        bstone::Log::write("SDL: Creating a software renderer...");

        ::sdl_soft_renderer = ::SDL_CreateRenderer(
            ::sdl_window,
            -1,
            SDL_RENDERER_SOFTWARE);

        if (!::sdl_soft_renderer) {
            is_succeed = false;
            bstone::Log::write_error(::SDL_GetError());
        }
    }

    int sdl_result = 0;

    if (is_succeed) {
        is_succeed = ::soft_initialize_textures();
    }

    if (is_succeed) {
        bstone::Log::write("SDL: Clearing default renderer target...");

        sdl_result = ::SDL_RenderClear(::sdl_soft_renderer);

        if (sdl_result != 0) {
            is_succeed = false;

            bstone::Log::write_error(::SDL_GetError());
        }
    }

    if (is_succeed) {
        is_succeed = ::soft_update_viewport();
    }

    if (is_succeed) {
        bstone::Log::write("SDL: Initializing default draw color...");

        sdl_result = ::SDL_SetRenderDrawColor(
            ::sdl_soft_renderer, 0, 0, 0, 255);

        if (sdl_result != 0) {
            is_succeed = false;

            bstone::Log::write_error(::SDL_GetError());
        }
    }

    return is_succeed;
}

bool x_initialize_video()
{
    bool is_succeed = true;

    if (is_succeed) {
        is_succeed = ::vid_pre_window_creation();
    }

    if (is_succeed) {
        bstone::Log::write("SDL: Creating a window...");

        if (!::sdl_use_custom_window_position) {
            ::sdl_window_x = (::display_mode.w - ::window_width) / 2;
            ::sdl_window_y = (::display_mode.h - ::window_height) / 2;
        }

        if (::sdl_window_x < 0) {
            ::sdl_window_x = 0;
        }

        if (::sdl_window_y < 0) {
            ::sdl_window_y = 0;
        }

        uint32_t flags = 0;

        flags |= ::vid_get_window_flags();

        if (!::sdl_is_windowed) {
            flags |=
                SDL_WINDOW_BORDERLESS |
                SDL_WINDOW_FULLSCREEN_DESKTOP;
        }

        std::string title = "Blake Stone: ???";

        if (::is_aog_full()) {
            std::string version_string;

            if (::is_aog_full_v1_0()) {
                version_string = "v1.0";
            } else if (::is_aog_full_v2_x()) {
                version_string = "v2.x";
            } else if (::is_aog_full_v3_0()) {
                version_string = "v3.0";
            }

            title = "Blake Stone: Aliens of Gold (full";

            if (!version_string.empty()) {
                title += ", " + version_string;
            }

            title += ')';
        } else if (::is_aog_sw()) {
            title = "Blake Stone: Aliens of Gold (shareware, v3.0)";
        } else if (::is_ps()) {
            title = "Blake Stone: Planet Strike (full, v1.x)";
        }

        ::sdl_window = ::SDL_CreateWindow(
            title.c_str(),
            ::sdl_window_x,
            ::sdl_window_y,
            ::window_width,
            ::window_height,
            flags);

        if (!sdl_window) {
            is_succeed = false;
            bstone::Log::write_error(::SDL_GetError());
        }
    }

    if (is_succeed) {
        is_succeed = ::vid_initialize_renderer();
    }

    if (is_succeed) {
        return true;
    }

    ::uninitialize_video();

    return false;
}

void initialize_video()
{
    bool is_custom_scale = false;

    //
    // Option "vid_windowed"
    //

    ::sdl_is_windowed = ::g_args.has_option("vid_windowed");

    ::sdl_use_custom_window_position = false;


    //
    // Option "vid_window_x"
    //

    auto&& vid_window_x_str = g_args.get_option_value("vid_window_x");

    if (bstone::StringHelper::lexical_cast(vid_window_x_str, ::sdl_window_x)) {
        ::sdl_use_custom_window_position = true;
    }


    //
    // Option "vid_window_y"
    //

    auto&& vid_window_y_str = g_args.get_option_value("vid_window_y");

    if (bstone::StringHelper::lexical_cast(vid_window_y_str, ::sdl_window_y)) {
        ::sdl_use_custom_window_position = true;
    }


    //
    // Option "vid_mode"
    //

    std::string width_str;
    std::string height_str;

    g_args.get_option_values("vid_mode", width_str, height_str);

    static_cast<void>(bstone::StringHelper::lexical_cast(
        width_str, ::window_width));

    static_cast<void>(bstone::StringHelper::lexical_cast(
        height_str, ::window_height));

    if (::window_width < ::vga_ref_width) {
        ::window_width = ::default_window_width;
    }

    if (::window_height < ::vga_ref_height) {
        ::window_height = ::default_window_height;
    }


    //
    // Option "vid_scale"
    //

    auto&& vid_scale_str = g_args.get_option_value("vid_scale");

    if (!vid_scale_str.empty()) {
        int scale = 0;

        if (bstone::StringHelper::lexical_cast(vid_scale_str, scale)) {
            if (scale < 1) {
                scale = 1;
            }

            ::vga_scale = scale;
            is_custom_scale = true;
        }
    }


    //
    // Renderer initialization
    //

    ::g_renderer_type = RT_NONE;

    auto&& ren_string = g_args.get_option_value("vid_renderer");

    if (!ren_string.empty()) {
        if (ren_string == "soft") {
            ::g_renderer_type = RT_SOFTWARE;
        } else if (ren_string == "ogl") {
            ::g_renderer_type = RT_OPEN_GL;
        } else {
            bstone::Log::write_warning(
                "CL: Unknown renderer type: {}",
                ren_string);
        }
    }

    if (::g_renderer_type == RT_NONE) {
        ::g_renderer_type = RT_AUTO_DETECT;
    }


    switch (::g_renderer_type) {
    case RT_AUTO_DETECT:
        ::g_renderer_type = RT_OPEN_GL;

    case RT_OPEN_GL:
        ::vid_pre_subsystem_creation = ::ogl_pre_subsystem_creation;
        ::vid_pre_window_creation = ::ogl_pre_window_creation;
        ::vid_get_window_flags = ::ogl_get_window_flags;
        ::vid_initialize_renderer = ::ogl_initialize_renderer;
        ::vid_draw_screen = ::ogl_draw_screen;
        ::vid_update_viewport = ::ogl_update_viewport;
        ::vid_refresh_screen = ::ogl_refresh_screen;
        ::vid_update_screen = ::ogl_update_screen;
        ::vid_uninitialize_video = ::ogl_uninitialize_video;
        break;

    case RT_SOFTWARE:
        ::vid_pre_subsystem_creation = ::soft_pre_subsystem_creation;
        ::vid_pre_window_creation = ::soft_pre_window_creation;
        ::vid_get_window_flags = ::soft_get_window_flags;
        ::vid_initialize_renderer = ::soft_initialize_renderer;
        ::vid_draw_screen = ::soft_draw_screen;
        ::vid_update_viewport = ::soft_update_viewport;
        ::vid_refresh_screen = ::soft_refresh_screen;
        ::vid_update_screen = ::soft_update_screen;
        ::vid_uninitialize_video = ::soft_uninitialize_video;
        break;

    default:
        throw std::runtime_error("Invalid renderer type.");
    }

    int sdl_result = 0;

    if (!::vid_pre_subsystem_creation()) {
        ::Quit("Failed to pre-initialize video subsystem.");
    }

    bstone::Log::write("SDL: Setting up a video subsystem...");

    sdl_result = ::SDL_GetDesktopDisplayMode(0, &::display_mode);

    if (sdl_result != 0) {
        ::Quit("SDL: Failed to get a display mode.");
    }

    if (!::sdl_is_windowed) {
        ::window_width = ::display_mode.w;
        ::window_height = ::display_mode.h;
    }

    ::vga_width = 0;
    ::vga_height = 0;

    // Force 4:3 aspect ratio
    double ar_correction = 1.2;

    if (is_custom_scale) {
        ::vga_width = ::vga_scale * ::vga_ref_width;
        ::vga_height = ::vga_scale * ::vga_ref_height;
    } else {
        ::vga_scale = 0;

        while (::vga_width < ::window_width ||
            (::vga_height * ar_correction) < ::window_height)
        {
            ++::vga_scale;
            ::vga_width += ::vga_ref_width;
            ::vga_height += ::vga_ref_height;
        }
    }

    ::vga_area = ::vga_width * ::vga_height;

    double h_scale = static_cast<double>(::window_width) / ::vga_width;
    double v_scale = ::window_height / (ar_correction * ::vga_height);

    double scale;

    if (h_scale <= v_scale) {
        scale = h_scale;
    } else {
        scale = v_scale;
    }

    ::screen_width = static_cast<int>(
        (::vga_width * scale) + 0.5);

    ::screen_height = static_cast<int>(
        (::vga_height * scale * ar_correction) + 0.5);

    ::screen_x = (::window_width - ::screen_width) / 2;
    ::screen_y = (::window_height - ::screen_height) / 2;

    ::VL_SetLineWidth(40);


    bool is_succeed = true;

    is_succeed = ::x_initialize_video();

    if (!is_succeed && ::g_renderer_type != RT_SOFTWARE) {
        bstone::Log::write_warning("SDL: Falling back to software renderer...");

        ::g_renderer_type = RT_SOFTWARE;

        ::vid_pre_subsystem_creation = ::soft_pre_subsystem_creation;
        ::vid_pre_window_creation = ::soft_pre_window_creation;
        ::vid_get_window_flags = ::soft_get_window_flags;
        ::vid_initialize_renderer = ::soft_initialize_renderer;
        ::vid_draw_screen = ::soft_draw_screen;
        ::vid_update_viewport = ::soft_update_viewport;
        ::vid_refresh_screen = ::soft_refresh_screen;
        ::vid_update_screen = ::soft_update_screen;
        ::vid_uninitialize_video = ::soft_uninitialize_video;

        is_succeed = ::x_initialize_video();
    }

    if (!is_succeed) {
        ::Quit("SDL: Failed to initialize a renderer.");
    }


    if (::g_renderer_type == RT_OPEN_GL) {
        sdl_result = ::SDL_GL_SetSwapInterval(1);

        if (sdl_result != 0) {
            bstone::Log::write_warning("OGL: Failed to enable VSync.");
        }
    }

    ::in_grab_mouse(true);
}

void uninitialize_video()
{
    if (::vid_uninitialize_video) {
        ::vid_uninitialize_video();
    }

    if (::sdl_window) {
        ::SDL_DestroyWindow(::sdl_window);
        ::sdl_window = nullptr;
    }
}


} // namespace


void JM_VGALinearFill(
    int start,
    int length,
    uint8_t fill)
{
    std::uninitialized_fill_n(
        &::vga_memory[::vl_get_offset(start)],
        ::vga_scale * ::vga_scale * 4 * length,
        fill);
}

void VL_RefreshScreen()
{
    ::vid_refresh_screen();
}

void VH_UpdateScreen()
{
    ::vid_update_screen();
}

int vl_get_offset(
    int base_offset)
{
    return ::vga_scale * ::vga_scale * 4 * base_offset;
}

int vl_get_offset(
    int base_offset,
    int x,
    int y)
{
    return ::vga_scale *
        (::vga_scale * (4 * base_offset) + (y * ::vga_width) + x);
}

uint8_t vl_get_pixel(
    int base_offset,
    int x,
    int y)
{
    return ::vga_memory[::vl_get_offset(base_offset, x, y)];
}

void vl_minimize_fullscreen_window(
    bool value)
{
    if (value) {
        ::SDL_MinimizeWindow(::sdl_window);
    } else {
        ::SDL_RestoreWindow(::sdl_window);
    }
}

void vl_update_vid_stretch()
{
    static_cast<void>(::vid_update_viewport());
    ::vid_draw_screen();
}
// BBi
