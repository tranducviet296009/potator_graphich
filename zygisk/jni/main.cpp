#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

#include "zygisk.hpp"

#define LOG_TAG "PotatoGFX-KSU"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace potatogfx {

// Cấu trúc lưu thông số đồ họa của từng ứng dụng từ config.json
struct AppConfig {
    bool enabled = false;
    bool minecraft_filter = true;
    int max_texture_size = 32;      // 0 = Gốc (không giới hạn), 16 / 32 / 64 / 128
    float downscale_factor = 0.6f;  // 0.5 -> 1.0 (dùng cho Android 16 Game Overlay)
    bool disable_msaa = true;
    int target_fps = 120;
};

static AppConfig g_active_cfg;
static bool g_hooks_installed = false;
static std::mutex g_tex_mutex;

struct TexDim {
    GLsizei orig_w = 0;
    GLsizei orig_h = 0;
    GLsizei scaled_w = 0;
    GLsizei scaled_h = 0;
};

static std::unordered_map<GLuint, TexDim> g_scaled_textures;

// ============================================================================
// BỘ PARSER JSON NHẸ, AN TOÀN (KHÔNG PHỤ THUỘC THƯ VIỆN NGOÀI)
// ============================================================================
static void skip_ws(const std::string &s, size_t &i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
}

static size_t find_matching_brace(const std::string &s, size_t open_pos) {
    if (open_pos >= s.size() || s[open_pos] != '{') return std::string::npos;
    int depth = 0;
    bool in_str = false;
    for (size_t i = open_pos; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\')) {
            in_str = !in_str;
        } else if (!in_str) {
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) return i;
            }
        }
    }
    return std::string::npos;
}

static size_t find_json_key(const std::string &block, const std::string &key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = block.find(pattern, pos)) != std::string::npos) {
        size_t after = pos + pattern.size();
        skip_ws(block, after);
        if (after < block.size() && block[after] == ':') {
            return after + 1;
        }
        pos += pattern.size();
    }
    return std::string::npos;
}

static bool extract_bool(const std::string &block, const std::string &key, bool default_val) {
    size_t val_pos = find_json_key(block, key);
    if (val_pos == std::string::npos) return default_val;
    skip_ws(block, val_pos);
    if (block.compare(val_pos, 4, "true") == 0) return true;
    if (block.compare(val_pos, 5, "false") == 0) return false;
    return default_val;
}

static int extract_int(const std::string &block, const std::string &key, int default_val) {
    size_t val_pos = find_json_key(block, key);
    if (val_pos == std::string::npos) return default_val;
    skip_ws(block, val_pos);
    char *endptr = nullptr;
    long v = std::strtol(block.c_str() + val_pos, &endptr, 10);
    if (endptr == block.c_str() + val_pos) return default_val;
    return static_cast<int>(v);
}

static float extract_float(const std::string &block, const std::string &key, float default_val) {
    size_t val_pos = find_json_key(block, key);
    if (val_pos == std::string::npos) return default_val;
    skip_ws(block, val_pos);
    char *endptr = nullptr;
    float v = std::strtof(block.c_str() + val_pos, &endptr);
    if (endptr == block.c_str() + val_pos) return default_val;
    return v;
}

static bool load_config_for_package(const std::string &json_text, const std::string &pkg_name, AppConfig &out_cfg) {
    if (pkg_name.empty() || json_text.empty()) return false;

    bool global_enabled = extract_bool(json_text, "global_enabled", true);
    if (!global_enabled) {
        return false;
    }

    size_t apps_val_pos = find_json_key(json_text, "apps");
    if (apps_val_pos == std::string::npos) return false;
    skip_ws(json_text, apps_val_pos);
    size_t apps_end = find_matching_brace(json_text, apps_val_pos);
    if (apps_end == std::string::npos) return false;

    std::string apps_block = json_text.substr(apps_val_pos, apps_end - apps_val_pos + 1);
    size_t pkg_val_pos = find_json_key(apps_block, pkg_name);
    if (pkg_val_pos == std::string::npos) return false;
    skip_ws(apps_block, pkg_val_pos);
    size_t pkg_end = find_matching_brace(apps_block, pkg_val_pos);
    if (pkg_end == std::string::npos) return false;

    std::string pkg_block = apps_block.substr(pkg_val_pos, pkg_end - pkg_val_pos + 1);
    out_cfg.enabled = extract_bool(pkg_block, "enabled", false);
    if (!out_cfg.enabled) return false;

    out_cfg.minecraft_filter = extract_bool(pkg_block, "minecraft_filter", true);
    out_cfg.max_texture_size = extract_int(pkg_block, "max_texture_size", 32);
    out_cfg.downscale_factor = extract_float(pkg_block, "downscale_factor", 0.6f);
    out_cfg.disable_msaa = extract_bool(pkg_block, "disable_msaa", true);
    out_cfg.target_fps = extract_int(pkg_block, "target_fps", 120);

    return true;
}

static std::string read_config_file(zygisk::Api *api) {
    if (api != nullptr) {
        int dir_fd = api->getModuleDir();
        if (dir_fd >= 0) {
            int fd = openat(dir_fd, "config.json", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                std::string content;
                char buf[4096];
                ssize_t n;
                while ((n = read(fd, buf, sizeof(buf))) > 0) {
                    content.append(buf, static_cast<size_t>(n));
                }
                close(fd);
                close(dir_fd);
                if (!content.empty()) return content;
            } else {
                close(dir_fd);
            }
        }
    }

    const char *fallback_path = "/data/adb/modules/potatogfx_ksu/config.json";
    int fd = open(fallback_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::string content;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            content.append(buf, static_cast<size_t>(n));
        }
        close(fd);
        return content;
    }
    return "";
}

// ============================================================================
// CON TRỎ HÀM GỐC OPENGL ES & EGL
// ============================================================================
using PFN_glTexParameteri = void (*)(GLenum target, GLenum pname, GLint param);
using PFN_glTexParameterf = void (*)(GLenum target, GLenum pname, GLfloat param);
using PFN_glTexParameteriv = void (*)(GLenum target, GLenum pname, const GLint *params);
using PFN_glTexParameterfv = void (*)(GLenum target, GLenum pname, const GLfloat *params);
using PFN_glSamplerParameteri = void (*)(GLuint sampler, GLenum pname, GLint param);
using PFN_glSamplerParameterf = void (*)(GLuint sampler, GLenum pname, GLfloat param);
using PFN_glTexImage2D = void (*)(GLenum target, GLint level, GLint internalformat,
                                  GLsizei width, GLsizei height, GLint border,
                                  GLenum format, GLenum type, const void *pixels);
using PFN_glTexSubImage2D = void (*)(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                     GLsizei width, GLsizei height, GLenum format,
                                     GLenum type, const void *pixels);
using PFN_glTexStorage2D = void (*)(GLenum target, GLsizei levels, GLenum internalformat,
                                    GLsizei width, GLsizei height);
using PFN_glCompressedTexImage2D = void (*)(GLenum target, GLint level, GLenum internalformat,
                                            GLsizei width, GLsizei height, GLint border,
                                            GLsizei imageSize, const void *data);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay dpy, const EGLint *attrib_list,
                                           EGLConfig *configs, EGLint config_size,
                                           EGLint *num_config);
using PFN_eglGetProcAddress = __eglMustCastToProperFunctionPointerType (*)(const char *procname);

static PFN_glTexParameteri orig_glTexParameteri = nullptr;
static PFN_glTexParameterf orig_glTexParameterf = nullptr;
static PFN_glTexParameteriv orig_glTexParameteriv = nullptr;
static PFN_glTexParameterfv orig_glTexParameterfv = nullptr;
static PFN_glSamplerParameteri orig_glSamplerParameteri = nullptr;
static PFN_glSamplerParameterf orig_glSamplerParameterf = nullptr;
static PFN_glTexImage2D orig_glTexImage2D = nullptr;
static PFN_glTexSubImage2D orig_glTexSubImage2D = nullptr;
static PFN_glTexStorage2D orig_glTexStorage2D = nullptr;
static PFN_glCompressedTexImage2D orig_glCompressedTexImage2D = nullptr;
static PFN_eglChooseConfig orig_eglChooseConfig = nullptr;
static PFN_eglGetProcAddress orig_eglGetProcAddress = nullptr;

// ============================================================================
// HÀM TIỆN ÍCH XỬ LÝ PIXEL & TEXTURE DOWNSCALE AN TOÀN
// ============================================================================
static int get_bytes_per_pixel(GLenum format, GLenum type) {
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
            case GL_RGBA:
                return 4;
            case GL_RGB:
                return 3;
            case GL_LUMINANCE_ALPHA:
            case GL_RG:
                return 2;
            case GL_ALPHA:
            case GL_LUMINANCE:
            case GL_RED:
                return 1;
            default:
                return 0;
        }
    } else if (type == GL_UNSIGNED_SHORT_5_6_5 ||
               type == GL_UNSIGNED_SHORT_4_4_4_4 ||
               type == GL_UNSIGNED_SHORT_5_5_5_1) {
        return 2;
    }
    return 0;
}

static void downsample_pixels_nearest(const uint8_t *src, int src_w, int src_h,
                                      uint8_t *dst, int dst_w, int dst_h, int bpp) {
    for (int y = 0; y < dst_h; ++y) {
        int src_y = (y * src_h) / dst_h;
        if (src_y >= src_h) src_y = src_h - 1;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = (x * src_w) / dst_w;
            if (src_x >= src_w) src_x = src_w - 1;
            const uint8_t *p_src = src + (src_y * src_w + src_x) * bpp;
            uint8_t *p_dst = dst + (y * dst_w + x) * bpp;
            for (int b = 0; b < bpp; ++b) {
                p_dst[b] = p_src[b];
            }
        }
    }
}

static void pixelate_buffer_to_grid(const uint8_t *src, uint8_t *dst,
                                    int width, int height, int bpp, int max_grid_cells) {
    if (max_grid_cells <= 0) return;
    int block_w = std::max(1, width / max_grid_cells);
    int block_h = std::max(1, height / max_grid_cells);
    if (block_w <= 1 && block_h <= 1) {
        std::memcpy(dst, src, static_cast<size_t>(width) * height * bpp);
        return;
    }

    for (int y = 0; y < height; y += block_h) {
        int sample_y = std::min(height - 1, y + block_h / 2);
        int max_by = std::min(height, y + block_h);
        for (int x = 0; x < width; x += block_w) {
            int sample_x = std::min(width - 1, x + block_w / 2);
            int max_bx = std::min(width, x + block_w);
            const uint8_t *center_px = src + (sample_y * width + sample_x) * bpp;

            for (int by = y; by < max_by; ++by) {
                uint8_t *row_dst = dst + (by * width + x) * bpp;
                for (int bx = x; bx < max_bx; ++bx) {
                    for (int b = 0; b < bpp; ++b) {
                        row_dst[b] = center_px[b];
                    }
                    row_dst += bpp;
                }
            }
        }
    }
}

static GLint convert_filter_to_nearest(GLint param) {
    switch (param) {
        case GL_LINEAR:
        case GL_NEAREST:
            return GL_NEAREST;
        case GL_LINEAR_MIPMAP_LINEAR:
        case GL_LINEAR_MIPMAP_NEAREST:
        case GL_NEAREST_MIPMAP_LINEAR:
        case GL_NEAREST_MIPMAP_NEAREST:
            return GL_NEAREST_MIPMAP_NEAREST;
        default:
            return param;
    }
}

// ============================================================================
// CÁC HÀM HOOK OPENGL ES & EGL
// ============================================================================
static void hook_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (!orig_glTexParameteri) return;
    if (g_active_cfg.minecraft_filter) {
        if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
            param = (pname == GL_TEXTURE_MAG_FILTER) ? GL_NEAREST : convert_filter_to_nearest(param);
        }
        if (pname == 0x84FE) {
            param = 1;
        }
    }
    orig_glTexParameteri(target, pname, param);
}

static void hook_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    if (!orig_glTexParameterf) return;
    if (g_active_cfg.minecraft_filter) {
        if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
            GLint iparam = static_cast<GLint>(param);
            iparam = (pname == GL_TEXTURE_MAG_FILTER) ? GL_NEAREST : convert_filter_to_nearest(iparam);
            param = static_cast<GLfloat>(iparam);
        }
        if (pname == 0x84FE) {
            param = 1.0f;
        }
    }
    orig_glTexParameterf(target, pname, param);
}

static void hook_glTexParameteriv(GLenum target, GLenum pname, const GLint *params) {
    if (!orig_glTexParameteriv) return;
    if (params != nullptr && g_active_cfg.minecraft_filter) {
        if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
            GLint modified = (pname == GL_TEXTURE_MAG_FILTER) ? GL_NEAREST : convert_filter_to_nearest(params[0]);
            orig_glTexParameteriv(target, pname, &modified);
            return;
        }
        if (pname == 0x84FE) {
            GLint modified = 1;
            orig_glTexParameteriv(target, pname, &modified);
            return;
        }
    }
    orig_glTexParameteriv(target, pname, params);
}

static void hook_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    if (!orig_glTexParameterfv) return;
    if (params != nullptr && g_active_cfg.minecraft_filter) {
        if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
            GLint iparam = static_cast<GLint>(params[0]);
            iparam = (pname == GL_TEXTURE_MAG_FILTER) ? GL_NEAREST : convert_filter_to_nearest(iparam);
            GLfloat modified = static_cast<GLfloat>(iparam);
            orig_glTexParameterfv(target, pname, &modified);
            return;
        }
        if (pname == 0x84FE) {
            GLfloat modified = 1.0f;
            orig_glTexParameterfv(target, pname, &modified);
            return;
        }
    }
    orig_glTexParameterfv(target, pname, params);
}

static void hook_glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
    if (!orig_glSamplerParameteri) return;
    if (g_active_cfg.minecraft_filter) {
        if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
            param = (pname == GL_TEXTURE_MAG_FILTER) ? GL_NEAREST : convert_filter_to_nearest(param);
        }
        if (pname == 0x84FE) {
            param = 1;
        }
    }
    orig_glSamplerParameteri(sampler, pname, param);
}

static void hook_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
    if (!orig_glSamplerParameterf) return;
    if (g_active_cfg.minecraft_filter) {
        if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
            GLint iparam = static_cast<GLint>(param);
            iparam = (pname == GL_TEXTURE_MAG_FILTER) ? GL_NEAREST : convert_filter_to_nearest(iparam);
            param = static_cast<GLfloat>(iparam);
        }
        if (pname == 0x84FE) {
            param = 1.0f;
        }
    }
    orig_glSamplerParameterf(sampler, pname, param);
}

static void hook_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                              GLsizei width, GLsizei height, GLint border,
                              GLenum format, GLenum type, const void *pixels) {
    if (!orig_glTexImage2D) return;

    const int max_tex = g_active_cfg.max_texture_size;
    if (target == GL_TEXTURE_2D && max_tex > 0 && pixels != nullptr && width > 64 && height > 64 &&
        (width > max_tex || height > max_tex)) {
        int bpp = get_bytes_per_pixel(format, type);
        if (bpp > 0) {
            GLint unpack_align = 4;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align);
            GLint unpack_row_len = 0;
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_len);

            if (unpack_row_len == 0 && ((width * bpp) % unpack_align == 0)) {
                if (level == 0) {
                    GLsizei new_w = std::max(1, std::min(width, max_tex));
                    GLsizei new_h = std::max(1, std::min(height, max_tex));

                    if (width > height) {
                        new_w = max_tex;
                        new_h = std::max(1, (height * max_tex) / width);
                    } else {
                        new_h = max_tex;
                        new_w = std::max(1, (width * max_tex) / height);
                    }

                    std::vector<uint8_t> scaled_buf(static_cast<size_t>(new_w) * new_h * bpp);
                    downsample_pixels_nearest(static_cast<const uint8_t *>(pixels),
                                              width, height,
                                              scaled_buf.data(), new_w, new_h, bpp);

                    GLint bound_tex = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex);
                    if (bound_tex > 0) {
                        std::lock_guard<std::mutex> lock(g_tex_mutex);
                        g_scaled_textures[static_cast<GLuint>(bound_tex)] = {width, height, new_w, new_h};
                    }

                    orig_glTexImage2D(target, level, internalformat, new_w, new_h, border,
                                      format, type, scaled_buf.data());
                    if (g_active_cfg.minecraft_filter && orig_glTexParameteri) {
                        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    }
                    return;
                } else {
                    int mip_grid = std::max(1, max_tex >> level);
                    std::vector<uint8_t> pix_buf(static_cast<size_t>(width) * height * bpp);
                    pixelate_buffer_to_grid(static_cast<const uint8_t *>(pixels), pix_buf.data(),
                                            width, height, bpp, mip_grid);
                    orig_glTexImage2D(target, level, internalformat, width, height, border,
                                      format, type, pix_buf.data());
                    return;
                }
            }
        }
    }

    orig_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    if (target == GL_TEXTURE_2D && g_active_cfg.minecraft_filter && orig_glTexParameteri) {
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
}

static void hook_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height, GLenum format,
                                 GLenum type, const void *pixels) {
    if (!orig_glTexSubImage2D) return;
    if (pixels == nullptr) {
        orig_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
        return;
    }

    const int max_tex = g_active_cfg.max_texture_size;
    if (target == GL_TEXTURE_2D && max_tex > 0) {
        int bpp = get_bytes_per_pixel(format, type);
        if (bpp > 0) {
            GLint bound_tex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex);

            TexDim dim_info{};
            bool is_scaled_tex = false;
            if (bound_tex > 0 && level == 0) {
                std::lock_guard<std::mutex> lock(g_tex_mutex);
                auto it = g_scaled_textures.find(static_cast<GLuint>(bound_tex));
                if (it != g_scaled_textures.end()) {
                    dim_info = it->second;
                    is_scaled_tex = true;
                }
            }

            if (is_scaled_tex && dim_info.orig_w > 0 && dim_info.orig_h > 0) {
                GLint new_x = (xoffset * dim_info.scaled_w) / dim_info.orig_w;
                GLint new_y = (yoffset * dim_info.scaled_h) / dim_info.orig_h;
                GLsizei new_w = std::max(1, (width * dim_info.scaled_w) / dim_info.orig_w);
                GLsizei new_h = std::max(1, (height * dim_info.scaled_h) / dim_info.orig_h);

                if (new_x + new_w > dim_info.scaled_w) new_w = dim_info.scaled_w - new_x;
                if (new_y + new_h > dim_info.scaled_h) new_h = dim_info.scaled_h - new_y;

                if (new_w > 0 && new_h > 0) {
                    std::vector<uint8_t> scaled_sub(static_cast<size_t>(new_w) * new_h * bpp);
                    downsample_pixels_nearest(static_cast<const uint8_t *>(pixels),
                                              width, height,
                                              scaled_sub.data(), new_w, new_h, bpp);
                    orig_glTexSubImage2D(target, level, new_x, new_y, new_w, new_h,
                                         format, type, scaled_sub.data());
                }
                return;
            }

            if (width > 128 && height > 128 && (width > max_tex || height > max_tex)) {
                GLint unpack_align = 4;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align);
                GLint unpack_row_len = 0;
                glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_len);
                if (unpack_row_len == 0 && ((width * bpp) % unpack_align == 0)) {
                    int grid_cells = std::max(1, max_tex >> level);
                    std::vector<uint8_t> pix_buf(static_cast<size_t>(width) * height * bpp);
                    pixelate_buffer_to_grid(static_cast<const uint8_t *>(pixels), pix_buf.data(),
                                            width, height, bpp, grid_cells);
                    orig_glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                                         format, type, pix_buf.data());
                    return;
                }
            }
        }
    }

    orig_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

static void hook_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                GLsizei width, GLsizei height) {
    if (!orig_glTexStorage2D) return;
    orig_glTexStorage2D(target, levels, internalformat, width, height);
    if (target == GL_TEXTURE_2D && g_active_cfg.minecraft_filter && orig_glTexParameteri) {
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                             (levels > 1) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
}

static void hook_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                        GLsizei width, GLsizei height, GLint border,
                                        GLsizei imageSize, const void *data) {
    if (!orig_glCompressedTexImage2D) return;
    orig_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);

    if (target == GL_TEXTURE_2D && orig_glTexParameteri) {
        if (g_active_cfg.minecraft_filter) {
            orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                 (level > 0) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
            orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        const int max_tex = g_active_cfg.max_texture_size;
        if (max_tex > 0 && level > 0 && width >= 16 && height >= 16) {
            if (width <= max_tex * 2 && height <= max_tex * 2) {
                orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level);
            }
        }
    }
}

static EGLBoolean hook_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                       EGLConfig *configs, EGLint config_size,
                                       EGLint *num_config) {
    if (!orig_eglChooseConfig) return EGL_FALSE;

    if (g_active_cfg.disable_msaa && attrib_list != nullptr) {
        std::vector<EGLint> patched_attribs;
        for (int i = 0; attrib_list[i] != EGL_NONE; i += 2) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i + 1];
            if (attr == EGL_SAMPLES || attr == EGL_SAMPLE_BUFFERS) {
                val = 0;
            }
            patched_attribs.push_back(attr);
            patched_attribs.push_back(val);
            if (i > 256) break;
        }
        patched_attribs.push_back(EGL_NONE);
        return orig_eglChooseConfig(dpy, patched_attribs.data(), configs, config_size, num_config);
    }

    return orig_eglChooseConfig(dpy, attrib_list, configs, config_size, num_config);
}

static __eglMustCastToProperFunctionPointerType hook_eglGetProcAddress(const char *procname) {
    if (!orig_eglGetProcAddress) return nullptr;
    auto real_fn = orig_eglGetProcAddress(procname);
    if (procname == nullptr || real_fn == nullptr) return real_fn;

    if (std::strcmp(procname, "glTexParameteri") == 0) {
        if (!orig_glTexParameteri) orig_glTexParameteri = reinterpret_cast<PFN_glTexParameteri>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexParameteri);
    }
    if (std::strcmp(procname, "glTexParameterf") == 0) {
        if (!orig_glTexParameterf) orig_glTexParameterf = reinterpret_cast<PFN_glTexParameterf>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexParameterf);
    }
    if (std::strcmp(procname, "glTexParameteriv") == 0) {
        if (!orig_glTexParameteriv) orig_glTexParameteriv = reinterpret_cast<PFN_glTexParameteriv>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexParameteriv);
    }
    if (std::strcmp(procname, "glTexParameterfv") == 0) {
        if (!orig_glTexParameterfv) orig_glTexParameterfv = reinterpret_cast<PFN_glTexParameterfv>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexParameterfv);
    }
    if (std::strcmp(procname, "glSamplerParameteri") == 0) {
        if (!orig_glSamplerParameteri) orig_glSamplerParameteri = reinterpret_cast<PFN_glSamplerParameteri>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glSamplerParameteri);
    }
    if (std::strcmp(procname, "glSamplerParameterf") == 0) {
        if (!orig_glSamplerParameterf) orig_glSamplerParameterf = reinterpret_cast<PFN_glSamplerParameterf>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glSamplerParameterf);
    }
    if (std::strcmp(procname, "glTexImage2D") == 0) {
        if (!orig_glTexImage2D) orig_glTexImage2D = reinterpret_cast<PFN_glTexImage2D>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexImage2D);
    }
    if (std::strcmp(procname, "glTexSubImage2D") == 0) {
        if (!orig_glTexSubImage2D) orig_glTexSubImage2D = reinterpret_cast<PFN_glTexSubImage2D>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexSubImage2D);
    }
    if (std::strcmp(procname, "glTexStorage2D") == 0) {
        if (!orig_glTexStorage2D) orig_glTexStorage2D = reinterpret_cast<PFN_glTexStorage2D>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glTexStorage2D);
    }
    if (std::strcmp(procname, "glCompressedTexImage2D") == 0) {
        if (!orig_glCompressedTexImage2D) orig_glCompressedTexImage2D = reinterpret_cast<PFN_glCompressedTexImage2D>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_glCompressedTexImage2D);
    }
    if (std::strcmp(procname, "eglChooseConfig") == 0) {
        if (!orig_eglChooseConfig) orig_eglChooseConfig = reinterpret_cast<PFN_eglChooseConfig>(real_fn);
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(hook_eglChooseConfig);
    }

    return real_fn;
}

// ============================================================================
// CƠ CHẾ CÀI ĐẶT HOOK: ZYGISK PLT HOOK + EGLGETPROCADDRESS HOOK
// ============================================================================
static void resolve_original_gl_symbols() {
    void *h_gles2 = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!h_gles2) h_gles2 = dlopen("libGLESv2.so", RTLD_LAZY);
    void *h_gles3 = dlopen("libGLESv3.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!h_gles3) h_gles3 = dlopen("libGLESv3.so", RTLD_LAZY);
    void *h_egl = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!h_egl) h_egl = dlopen("libEGL.so", RTLD_LAZY);

    if (h_gles2) {
        if (!orig_glTexParameteri) orig_glTexParameteri = reinterpret_cast<PFN_glTexParameteri>(dlsym(h_gles2, "glTexParameteri"));
        if (!orig_glTexParameterf) orig_glTexParameterf = reinterpret_cast<PFN_glTexParameterf>(dlsym(h_gles2, "glTexParameterf"));
        if (!orig_glTexParameteriv) orig_glTexParameteriv = reinterpret_cast<PFN_glTexParameteriv>(dlsym(h_gles2, "glTexParameteriv"));
        if (!orig_glTexParameterfv) orig_glTexParameterfv = reinterpret_cast<PFN_glTexParameterfv>(dlsym(h_gles2, "glTexParameterfv"));
        if (!orig_glSamplerParameteri) orig_glSamplerParameteri = reinterpret_cast<PFN_glSamplerParameteri>(dlsym(h_gles2, "glSamplerParameteri"));
        if (!orig_glSamplerParameterf) orig_glSamplerParameterf = reinterpret_cast<PFN_glSamplerParameterf>(dlsym(h_gles2, "glSamplerParameterf"));
        if (!orig_glTexImage2D) orig_glTexImage2D = reinterpret_cast<PFN_glTexImage2D>(dlsym(h_gles2, "glTexImage2D"));
        if (!orig_glTexSubImage2D) orig_glTexSubImage2D = reinterpret_cast<PFN_glTexSubImage2D>(dlsym(h_gles2, "glTexSubImage2D"));
        if (!orig_glTexStorage2D) orig_glTexStorage2D = reinterpret_cast<PFN_glTexStorage2D>(dlsym(h_gles2, "glTexStorage2D"));
        if (!orig_glCompressedTexImage2D) orig_glCompressedTexImage2D = reinterpret_cast<PFN_glCompressedTexImage2D>(dlsym(h_gles2, "glCompressedTexImage2D"));
    }
    if (h_gles3) {
        if (!orig_glSamplerParameteri) orig_glSamplerParameteri = reinterpret_cast<PFN_glSamplerParameteri>(dlsym(h_gles3, "glSamplerParameteri"));
        if (!orig_glSamplerParameterf) orig_glSamplerParameterf = reinterpret_cast<PFN_glSamplerParameterf>(dlsym(h_gles3, "glSamplerParameterf"));
        if (!orig_glTexStorage2D) orig_glTexStorage2D = reinterpret_cast<PFN_glTexStorage2D>(dlsym(h_gles3, "glTexStorage2D"));
    }
    if (h_egl) {
        if (!orig_eglChooseConfig) orig_eglChooseConfig = reinterpret_cast<PFN_eglChooseConfig>(dlsym(h_egl, "eglChooseConfig"));
        if (!orig_eglGetProcAddress) orig_eglGetProcAddress = reinterpret_cast<PFN_eglGetProcAddress>(dlsym(h_egl, "eglGetProcAddress"));
    }
}

static void register_plt_hooks_from_maps(zygisk::Api *api) {
    if (!api) return;
    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (!fp) return;

    struct LibInode {
        dev_t dev;
        ino_t ino;
    };
    std::vector<LibInode> seen;
    char line[1024];

    while (std::fgets(line, sizeof(line), fp)) {
        unsigned int dev_major = 0, dev_minor = 0;
        unsigned long inode = 0;
        char perms[8] = {0};
        int path_offset = 0;

        if (std::sscanf(line, "%*x-%*x %7s %*x %x:%x %lu %n",
                        perms, &dev_major, &dev_minor, &inode, &path_offset) >= 4) {
            if (inode == 0 || path_offset <= 0) continue;
            const char *path = line + path_offset;
            if (std::strstr(path, ".so") == nullptr) continue;
            if (std::strstr(path, "potatogfx") != nullptr) continue;

            dev_t dev = makedev(dev_major, dev_minor);
            ino_t ino = static_cast<ino_t>(inode);

            bool exists = false;
            for (const auto &item : seen) {
                if (item.dev == dev && item.ino == ino) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;
            seen.push_back({dev, ino});

            api->pltHookRegister(dev, ino, "glTexParameteri", reinterpret_cast<void *>(hook_glTexParameteri), reinterpret_cast<void **>(&orig_glTexParameteri));
            api->pltHookRegister(dev, ino, "glTexParameterf", reinterpret_cast<void *>(hook_glTexParameterf), reinterpret_cast<void **>(&orig_glTexParameterf));
            api->pltHookRegister(dev, ino, "glTexParameteriv", reinterpret_cast<void *>(hook_glTexParameteriv), reinterpret_cast<void **>(&orig_glTexParameteriv));
            api->pltHookRegister(dev, ino, "glTexParameterfv", reinterpret_cast<void *>(hook_glTexParameterfv), reinterpret_cast<void **>(&orig_glTexParameterfv));
            api->pltHookRegister(dev, ino, "glSamplerParameteri", reinterpret_cast<void *>(hook_glSamplerParameteri), reinterpret_cast<void **>(&orig_glSamplerParameteri));
            api->pltHookRegister(dev, ino, "glSamplerParameterf", reinterpret_cast<void *>(hook_glSamplerParameterf), reinterpret_cast<void **>(&orig_glSamplerParameterf));
            api->pltHookRegister(dev, ino, "glTexImage2D", reinterpret_cast<void *>(hook_glTexImage2D), reinterpret_cast<void **>(&orig_glTexImage2D));
            api->pltHookRegister(dev, ino, "glTexSubImage2D", reinterpret_cast<void *>(hook_glTexSubImage2D), reinterpret_cast<void **>(&orig_glTexSubImage2D));
            api->pltHookRegister(dev, ino, "glTexStorage2D", reinterpret_cast<void *>(hook_glTexStorage2D), reinterpret_cast<void **>(&orig_glTexStorage2D));
            api->pltHookRegister(dev, ino, "glCompressedTexImage2D", reinterpret_cast<void *>(hook_glCompressedTexImage2D), reinterpret_cast<void **>(&orig_glCompressedTexImage2D));
            api->pltHookRegister(dev, ino, "eglChooseConfig", reinterpret_cast<void *>(hook_eglChooseConfig), reinterpret_cast<void **>(&orig_eglChooseConfig));
            api->pltHookRegister(dev, ino, "eglGetProcAddress", reinterpret_cast<void *>(hook_eglGetProcAddress), reinterpret_cast<void **>(&orig_eglGetProcAddress));
        }
    }
    std::fclose(fp);
    api->pltHookCommit();
}

static void install_all_gl_hooks(zygisk::Api *api, const char *pkg_name) {
    if (g_hooks_installed) return;
    g_hooks_installed = true;

    resolve_original_gl_symbols();
    register_plt_hooks_from_maps(api);

    LOGI("Đã kích hoạt PotatoGFX cho [%s] | Minecraft=%d | MaxTex=%dpx | MSAA_Off=%d | Scale=%.2f | FPS=%d",
         pkg_name,
         g_active_cfg.minecraft_filter ? 1 : 0,
         g_active_cfg.max_texture_size,
         g_active_cfg.disable_msaa ? 1 : 0,
         g_active_cfg.downscale_factor,
         g_active_cfg.target_fps);
}

// ============================================================================
// LỚP MODULE ZYGISK v4 CHÍNH
// ============================================================================
class PotatoGfxModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api_ = api;
        this->env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (args == nullptr || args->nice_name == nullptr) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        uint32_t flags = api_->getFlags();
        if ((flags & zygisk::StateFlags::PROCESS_ON_DENYLIST) != 0 || args->uid < 10000) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char *raw_nice_name = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (raw_nice_name != nullptr) {
            std::string proc_name(raw_nice_name);
            env_->ReleaseStringUTFChars(args->nice_name, raw_nice_name);

            size_t colon_pos = proc_name.find(':');
            if (colon_pos != std::string::npos) {
                package_name_ = proc_name.substr(0, colon_pos);
            } else {
                package_name_ = proc_name;
            }
        }

        std::string config_json = read_config_file(api_);
        should_hook_ = load_config_for_package(config_json, package_name_, target_cfg_);

        if (!should_hook_) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        std::string live_json = read_config_file(api_);
        if (!live_json.empty()) {
            AppConfig refreshed_cfg;
            if (load_config_for_package(live_json, package_name_, refreshed_cfg)) {
                should_hook_ = true;
                target_cfg_ = refreshed_cfg;
            } else {
                should_hook_ = false;
            }
        }

        if (!should_hook_ || !target_cfg_.enabled) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        g_active_cfg = target_cfg_;
        install_all_gl_hooks(api_, package_name_.c_str());
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    std::string package_name_;
    bool should_hook_ = false;
    AppConfig target_cfg_;
};

} // namespace potatogfx

REGISTER_ZYGISK_MODULE(potatogfx::PotatoGfxModule)
