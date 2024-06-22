#include "headers.h"
#include "gui_text.h"
#include "handler.h"

#include <http/chunked.h>

#include <sys/stat.h>

#include <mutex>

#include "ScreenHandler.hpp"

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

GUIText::GUIText(bool can_keep_alive, bool with_text_only, bool enabled_only, bool visible_only)
    : can_keep_alive(can_keep_alive)
    , with_text_only(with_text_only)
    , enabled_only(enabled_only)
    , visible_only(visible_only)
{
}

namespace {
    window_t* get_next_window(window_t* current) {
        // Window has children? Next window is first child.
        window_t* next = current->GetFirstSubWin();
        if (next != nullptr)
            return next;

        while(true) {
            // No children, or returning from children? Next window is next sibling.
            next = current->GetNext();
            if (next != nullptr)
                return next;

            // No next sibling? Move back to parent and get its next sibling
            current = current->GetParent();
            if (current == nullptr)
                return nullptr;
        }
    }

    inline bool put_buffer_byte(uint8_t*& buffer, uint8_t* end, uint8_t val) {
        if (buffer < end) {
            *buffer++ = val;
            return true;
        }
        return false;
    }

    inline bool put_buffer_short(uint8_t*& buffer, uint8_t* end, uint16_t val) {
        return put_buffer_byte(buffer, end, (val & 0xFF)) && put_buffer_byte(buffer, end, (val >> 8));
    }
}

Step GUIText::step(string_view, bool, uint8_t *buffer, size_t buffer_size) {
    ConnectionHandling handling = can_keep_alive ? ConnectionHandling::ChunkedKeep : ConnectionHandling::Close;

    size_t written = 0;
    NextInstruction instruction = Continue();
    if (!headers_sent) {
        written += write_headers(buffer, buffer_size, Status::Ok, ContentType::ApplicationOctetStream, handling, std::nullopt, std::nullopt);
        buffer_size -= written;
        buffer += written;
        headers_sent = true;

    } else {
        // Doing this all in one go.
        if (buffer_size >= MIN_CHUNK_SIZE) {
            written += http::render_chunk(handling, buffer, buffer_size, [&](uint8_t *buffer_, size_t buffer_size_) -> std::optional<size_t> {
                if (done) {
                    instruction = Terminating::for_handling(handling);
                    return 0;
                }

                std::lock_guard lock(gui_mutex);

                screen_t* root = Screens::Access()->Get();

                window_t* current = root;

                uint8_t* bpos = buffer_;
                uint8_t* bend = buffer_ + buffer_size_;

                int safety_check = 2000;

                do {
                    uint8_t flags = 0;
                    if (visible_only && !current->IsVisible())
                        continue;

                    if (enabled_only && !current->IsEnabled())
                        continue;

                    string_view_utf8 text = current->GetText();
                    StringReaderUtf8 reader(text);
                    uint8_t text_byte = reader.getbyte();

                    if (with_text_only && text_byte == 0)
                        continue;

                    if (current->IsEnabled())
                        flags |= 0x01;

                    if (current->IsVisible())
                        flags |= 0x02;

                    if (current->IsFocused())
                        flags |= 0x80;

                    auto rect = current->GetRect();
                    put_buffer_byte(bpos, bend, flags);

                    put_buffer_byte(bpos, bend, rect.Left() & 0xFF);
                    put_buffer_byte(bpos, bend, rect.Top() & 0xFF);
                    put_buffer_byte(bpos, bend, ((rect.Left() >> 4) & 0xF0) | ((rect.Top() >> 8) & 0x0F));

                    put_buffer_byte(bpos, bend, rect.Width() & 0xFF);
                    put_buffer_byte(bpos, bend, rect.Height() & 0xFF);
                    put_buffer_byte(bpos, bend, ((rect.Width() >> 4) & 0xF0) | ((rect.Height() >> 8) & 0x0F));

                    while (text_byte != 0) {
                        put_buffer_byte(bpos, bend, text_byte);
                        text_byte = reader.getbyte();
                    }

                    put_buffer_byte(bpos, bend, 0);

                } while (bpos < bend && (current = get_next_window(current)) != nullptr && --safety_check > 0);

                done = true;

                return bpos - buffer_;
            });
        }
    }
    return { 0, written, std::move(instruction) };
}

} // namespace nhttp::printer
