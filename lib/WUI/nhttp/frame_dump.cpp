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

#if HAS_ST7789_DISPLAY()
static const uint8_t bytes_per_pixel = 3;
static const uint8_t buffer_rows = 10;
static const uint8_t read_start_offset = 2;
    #include "st7789v.hpp"
#elif HAS_ILI9488_DISPLAY()
static const uint8_t bytes_per_pixel = 3;
static const uint8_t buffer_rows = ILI9488_BUFF_ROWS;
static const uint8_t read_start_offset = 0;
    #include "ili9488.hpp"
#endif // USE_ILI9488

FrameDump::FrameDump(bool can_keep_alive)
    : can_keep_alive(can_keep_alive)
{
}

int FrameDump::read_some_pixels(uint8_t *buffer, size_t buffer_size) {
    if (pos.y >= display::GetH()) {
        // done with dump
        return 0;
    }

    int pixels_remaining_in_line = display::GetW() - pos.x;
    int max_pixels_in_buffer = buffer_size / bytes_per_pixel;
    if (max_pixels_in_buffer == 0) {
        // not enough space in buffer for a single pixel?
        return 0;
    }

    int num_pixels = std::min(pixels_remaining_in_line, max_pixels_in_buffer);
    int num_bytes = num_pixels * bytes_per_pixel;

    std::lock_guard lock(gui_mutex);

    uint8_t *input_buffer = display::GetBlock(pos, end);
    if (input_buffer == NULL) {
        memset(buffer, 0, num_bytes);
    } else {
        memcpy(buffer, input_buffer + read_start_offset, num_bytes);
    }

    pos.x += num_pixels;
    if (pos.x >= display::GetW()) {
        pos.x = 0;
        pos.y += 1;
    }

    return num_bytes;
}

Step FrameDump::step(string_view, bool, uint8_t *buffer, size_t buffer_size) {
    ConnectionHandling handling = can_keep_alive ? ConnectionHandling::ChunkedKeep : ConnectionHandling::Close;

    size_t written = 0;
    NextInstruction instruction = Continue();
    if (!headers_sent) {
        written += write_headers(buffer, buffer_size, Status::Ok, ContentType::ApplicationOctetStream, handling, std::nullopt, std::nullopt);
        buffer_size -= written;
        buffer += written;
        headers_sent = true;

    }
    if (buffer_size >= MIN_CHUNK_SIZE) {
        written += http::render_chunk(handling, buffer, buffer_size, [&](uint8_t *buffer_, size_t buffer_size_) -> std::optional<size_t> {
            int bytes_written = 0;
            int bytes_read;

            if (pos.y >= display::GetH()) {
                // done with dump
                instruction = Terminating::for_handling(handling);
                return 0;
            }

            if (buffer_size_ < bytes_per_pixel) {
                return std::nullopt;
            }

            while ((bytes_read = read_some_pixels(buffer_, buffer_size_)) > 0) {
                bytes_written += bytes_read;
                buffer_size_ -= bytes_read;
                buffer_ += bytes_read;
            }

            return bytes_written;
        });
    }

    return { 0, written, std::move(instruction) };
}

} // namespace nhttp::printer
