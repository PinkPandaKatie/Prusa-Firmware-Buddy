#include "headers.h"
#include "gui_text.h"
#include "handler.h"

#include <http/chunked.h>

#include <sys/stat.h>

#include <mutex>

#include "ScreenHandler.hpp"

// for definition of Pixel
#include "ScreenShot.hpp"

using namespace http;

namespace nhttp::printer {

using handler::Continue;
using handler::NextInstruction;
using handler::StatusPage;
using handler::Step;
using handler::Terminating;
using http::ContentType;
using http::Status;
using std::nullopt;
using std::string_view;

// Copied from ScreenShot.cpp. Should probably be in a header somewhere.

#ifdef USE_ST7789
static const uint8_t bytes_per_pixel = 3;
static const uint8_t buffer_rows = 10;
static const uint8_t read_start_offset = 2;
    #include "st7789v.hpp"
#endif // USE_ST7789
#ifdef USE_ILI9488
static const uint8_t bytes_per_pixel = 3;
static const uint8_t buffer_rows = ILI9488_BUFF_ROWS;
static const uint8_t read_start_offset = 0;
    #include "ili9488.hpp"
#endif // USE_ILI9488


#define QOI_SRGB   0
#define QOI_LINEAR 1

#define QOI_OP_INDEX  0x00 /* 00xxxxxx */
#define QOI_OP_DIFF   0x40 /* 01xxxxxx */
#define QOI_OP_LUMA   0x80 /* 10xxxxxx */
#define QOI_OP_RUN    0xc0 /* 11xxxxxx */
#define QOI_OP_RGB    0xfe /* 11111110 */
#define QOI_OP_RGBA   0xff /* 11111111 */

#define QOI_MASK_2    0xc0 /* 11000000 */

#define QOI_MAGIC \
    (((unsigned int)'q') << 24 | ((unsigned int)'o') << 16 | \
     ((unsigned int)'i') <<  8 | ((unsigned int)'f'))
#define QOI_HEADER_SIZE 14

static uint8_t qoi_color_hash(const qoi_rgba_t& px) {
    return (px.rgba.r*3 + px.rgba.g*5 + px.rgba.b*7 + px.rgba.a*11) & 0x3F;
}

static void qoi_write_32(unsigned char*& buf, unsigned int v) {
    *buf++ = (0xff000000 & v) >> 24;
    *buf++ = (0x00ff0000 & v) >> 16;
    *buf++ = (0x0000ff00 & v) >> 8;
    *buf++ = (0x000000ff & v);
}

FrameDump::FrameDump(bool can_keep_alive, int debug_flags)
    : debug_flags(debug_flags)
    , can_keep_alive(can_keep_alive)
{
    qoi_run = 0;
    pos.x = 0;
    pos.y = 0;
    px_prev.rgba.r = 0;
    px_prev.rgba.g = 0;
    px_prev.rgba.b = 0;
    px_prev.rgba.a = 255;
    memset(qoi_index, 0, sizeof(qoi_index));
}

namespace {

    inline void next_pixel(uint8_t*& input_pos, point_ui16_t& pos) {
        input_pos += bytes_per_pixel;
        pos.x++;
        if (pos.x >= display::GetW()) {
            pos.x = 0;
            pos.y++;
        }
    }

}

int FrameDump::read_some_pixels(uint8_t *buffer, size_t buffer_size) {
    if (pos.y >= display::GetH()) {
        state = FrameDumpState::QOIEnd;

        // done with dump
        return 0;
    }

    int bytes_per_row = bytes_per_pixel * display::GetW();

    // We don't know how many pixels will fit in the output buffer after compression.

    point_ui16_t start(0, pos.y);
    point_ui16_t end = point_ui16(display::GetW() - 1, pos.y + buffer_rows - 1);

    if (end.y >= display::GetH())
        end.y = display::GetH() - 1;


    std::lock_guard lock(gui_mutex);

    uint8_t *input_buffer = display::GetBlock(start, end);

    if (input_buffer == NULL) {
        state = FrameDumpState::QOIEnd;

        pos.y = display::GetH();
        return 0;
    }

    input_buffer += read_start_offset;

    uint8_t* input_pos = input_buffer + (pos.x * bytes_per_pixel);
    uint8_t* input_end = (input_buffer + ((end.y - start.y + 1) * bytes_per_row)) - bytes_per_pixel;

    uint8_t* output_pos = buffer;
    uint8_t* output_end = buffer + buffer_size;

    qoi_rgba_t px;
    px.rgba.a = 255;

    while (input_pos <= input_end && output_pos < output_end) {
#if defined(USE_ST7789)
        px.rgba.r = input_pos[0];
        px.rgba.g = input_pos[1];
        px.rgba.b = input_pos[2];

#elif defined(USE_ILI9488)
        // color order is BGR
        px.rgba.b = input_pos[0] << 2;
        px.rgba.g = input_pos[1] << 2;
        px.rgba.r = input_pos[2] << 2;
#else
        px.rgba.r = 0;
        px.rgba.g = 0;
        px.rgba.b = 0;

#endif

        if (px.v == px_prev.v && !(debug_flags & 1)) {
            qoi_run++;
            if (qoi_run == 62 || input_pos == input_end) {
                *output_pos++ = QOI_OP_RUN | (qoi_run - 1);
                qoi_run = 0;
            }
        } else {
            int index_pos;
            int index_ofs;

            if (qoi_run > 0) {
                *output_pos++ = QOI_OP_RUN | (qoi_run - 1);
                qoi_run = 0;
            }

            index_pos = qoi_color_hash(px);
            index_ofs = index_pos * 3;

            if (
                px.rgba.r == qoi_index[index_ofs + 0] &&
                px.rgba.g == qoi_index[index_ofs + 1] &&
                px.rgba.b == qoi_index[index_ofs + 2] &&
                !(debug_flags & 2)) {
                if (output_pos >= output_end) break;
                *output_pos++ = QOI_OP_INDEX | index_pos;
            }
            else {
                signed char vr = px.rgba.r - px_prev.rgba.r;
                signed char vg = px.rgba.g - px_prev.rgba.g;
                signed char vb = px.rgba.b - px_prev.rgba.b;

                signed char vg_r = vr - vg;
                signed char vg_b = vb - vg;

                if (
                    vr > -3 && vr < 2 &&
                    vg > -3 && vg < 2 &&
                    vb > -3 && vb < 2 &&
                    !(debug_flags & 4)) {

                    if (output_pos >= output_end) break;
                    *output_pos++ = QOI_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2);
                }
                else if (
                    vg_r >  -9 && vg_r <  8 &&
                    vg   > -33 && vg   < 32 &&
                    vg_b >  -9 && vg_b <  8 &&
                    !(debug_flags & 8)) {

                    if ((output_end - output_pos) < 2) break;

                    *output_pos++ = QOI_OP_LUMA     | (vg   + 32);
                    *output_pos++ = (vg_r + 8) << 4 | (vg_b +  8);
                }
                else {
                    if ((output_end - output_pos) < 4) break;
                    *output_pos++ = QOI_OP_RGB;
                    *output_pos++ = px.rgba.r;
                    *output_pos++ = px.rgba.g;
                    *output_pos++ = px.rgba.b;
                }

                qoi_index[index_ofs + 0] = px.rgba.r;
                qoi_index[index_ofs + 1] = px.rgba.g;
                qoi_index[index_ofs + 2] = px.rgba.b;

            }
        }
        px_prev = px;

        next_pixel(input_pos, pos);
    }

    return output_pos - buffer;
}

Step FrameDump::step(string_view, bool, uint8_t *buffer, size_t buffer_size) {
    ConnectionHandling handling = can_keep_alive ? ConnectionHandling::ChunkedKeep : ConnectionHandling::Close;

    size_t written = 0;
    NextInstruction instruction = Continue();
    if (state == FrameDumpState::HTTPHeaders) {
        written += write_headers(buffer, buffer_size, Status::Ok, ContentType::ImageQoi, handling, std::nullopt, std::nullopt);
        buffer_size -= written;
        buffer += written;
        state = FrameDumpState::QOIHeader;
    }

    if (buffer_size >= MIN_CHUNK_SIZE) {
        written += http::render_chunk(handling, buffer, buffer_size, [&](uint8_t *buffer_, size_t buffer_size_) -> std::optional<size_t> {
            int bytes_written = 0;
            int bytes_read;

            while (true) {
                if (buffer_size_ < 8) {
                    if (!bytes_written)
                        return std::nullopt;
                    return bytes_written;
                }

                switch(state) {
                    case FrameDumpState::QOIHeader:
                        if (buffer_size_ < QOI_HEADER_SIZE)
                            return std::nullopt;

                        qoi_write_32(buffer_, QOI_MAGIC);
                        qoi_write_32(buffer_, display::GetW());
                        qoi_write_32(buffer_, display::GetH());
                        *buffer_++ = 3; // channels
                        *buffer_++ = QOI_SRGB;
                        bytes_written += QOI_HEADER_SIZE;
                        buffer_size_ -= QOI_HEADER_SIZE;
                        state = FrameDumpState::QOIData;

                        // fall through
                    case FrameDumpState::QOIData:
                        bytes_read = read_some_pixels(buffer_, buffer_size_);
                        bytes_written += bytes_read;
                        buffer_size_ -= bytes_read;
                        buffer_ += bytes_read;
                        break;

                    case FrameDumpState::QOIEnd:

                        memset(buffer_, 0, 7);
                        buffer_[7] = 1;
                        buffer_size_ -= 8;
                        bytes_written += 8;
                        state = FrameDumpState::Done;

                        // Return now instead of falling through, so we write a zero chunk
                        // next time through
                        return bytes_written;

                    case Done:
                    default:
                        instruction = Terminating::for_handling(handling);
                        return 0;
                }
            }

            return bytes_written;
        });
    }

    return { 0, written, std::move(instruction) };
}

} // namespace nhttp::printer
