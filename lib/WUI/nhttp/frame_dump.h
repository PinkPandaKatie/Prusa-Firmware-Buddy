#pragma once

#include "step.h"

#include "display.hpp"

#include <http/types.h>

#include <cstdio>
#include <string_view>
#include <memory>

namespace nhttp::printer {

typedef union {
	struct { unsigned char r, g, b, a; } rgba;
	unsigned int v;
} qoi_rgba_t;

class FrameDump {
private:
    enum FrameDumpState {
        HTTPHeaders,
        QOIHeader,
        QOIData,
        QOIEnd,
        Done
    };

    FrameDumpState state = FrameDumpState::HTTPHeaders;

    point_ui16_t pos;
    int qoi_run;
	qoi_rgba_t px_prev;
    uint8_t qoi_index[64 * 3];

    int debug_flags;
    bool can_keep_alive;

public:
    FrameDump(bool can_keep_alive, int debug_flags=0);
    bool want_read() const { return false; }
    bool want_write() const { return true; }
    int read_some_pixels(uint8_t *buffer, size_t buffer_size);
    void step(std::string_view input, bool terminated_by_client, uint8_t *buffer, size_t buffer_size, handler::Step& out);
};

} // namespace nhttp::printer
