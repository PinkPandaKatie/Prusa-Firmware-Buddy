#pragma once

#include "step.h"

#include "display.h"

#include <http/types.h>

#include <cstdio>
#include <string_view>
#include <memory>

namespace nhttp::printer {

class FrameDump {
private:
    point_ui16_t pos;
    bool headers_sent = false;
    bool done = false;
    bool can_keep_alive;

public:
    FrameDump(bool can_keep_alive);
    bool want_read() const { return false; }
    bool want_write() const { return true; }
    int read_some_pixels(uint8_t *buffer, size_t buffer_size);
    handler::Step step(std::string_view input, bool terminated_by_client, uint8_t *buffer, size_t buffer_size);
};

} // namespace nhttp::printer
