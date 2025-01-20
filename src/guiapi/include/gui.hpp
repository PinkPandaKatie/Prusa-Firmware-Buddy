// gui.hpp
#pragma once

#include "guitypes.hpp"
#include "gui_timer.h"
#include "display_helper.h"
#include "display.h"
#include <guiconfig/GuiDefaults.hpp>

#include <common/freertos_mutex.hpp>

extern void gui_run(void);

extern void gui_init(void);

extern void gui_redraw(void);

#include "window.hpp"
#include "window_frame.hpp"
#include "window_text.hpp"
#include "window_roll_text.hpp"
#include "window_numb.hpp"
#include "window_icon.hpp"
#include "window_term.hpp"
#include "window_msgbox.hpp"
#include "window_progress.hpp"
#include "window_qr.hpp"

extern uint8_t gui_get_nesting(void);

extern void gui_loop(void);
extern void gui_error_run(void);

extern void gui_bare_loop(void);

extern void gui_lock(void);
extern void gui_unlock(void);

enum GuiFakeEvent {
    None = 0,
    KnobClick,
    KnobLeft,
    KnobRight,
    ScreenTap
};

extern void gui_fake_input(GuiFakeEvent type);
extern void gui_fake_tap(point_ui16_t pos);

extern freertos::Mutex gui_mutex;

