#include "prusa_link_api_v1.h"
#include "basic_gets.h"
#include "../nhttp/file_info.h"
#include "../nhttp/file_command.h"
#include "../nhttp/gui_text.h"
#include "../nhttp/headers.h"
#include "../nhttp/gcode_upload.h"
#include "../nhttp/job_command.h"
#include "../nhttp/send_json.h"
#include "../nhttp/status_renderer.h"
#include "../wui_api.h"
#include "prusa_api_helpers.hpp"

#include <http/url_decode.h>
#include <marlin_client.hpp>
#include <common/path_utils.h>
#include <transfers/monitor.hpp>
#include <transfers/changed_path.hpp>
#include <state/printer_state.hpp>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <charconv>
#include <unistd.h>

#include "gui.hpp"

namespace nhttp::link_content {

using http::Method;
using http::Status;
using http::url_decode;
using std::nullopt;
using std::optional;
using std::string_view;
using namespace handler;
using namespace transfers;
using nhttp::printer::FileCommand;
using nhttp::printer::FileInfo;
using nhttp::printer::FrameDump;
using nhttp::printer::GUIText;
using nhttp::printer::GcodeUpload;
using nhttp::printer::JobCommand;
using printer_state::DeviceState;
using transfers::ChangedPath;

namespace {
    optional<int> get_job_id(string_view str) {
        int ret;
        auto result = from_chars_light(str.begin(), str.end(), ret);
        if (result.ec != std::errc {}) {
            return nullopt;
        }

        return ret;
    }

    void stop_print(const RequestParser &parser, handler::Step &out) {
        switch (printer_state::get_state(false)) {
        case DeviceState::Printing:
        case DeviceState::Paused:
        case DeviceState::Attention:
            marlin_client::print_abort();
            out.next = StatusPage(Status::NoContent, parser);
            return;
        default:
            out.next = StatusPage(Status::Conflict, parser);
            return;
        }
    }

    void pause_print(const RequestParser &parser, handler::Step &out) {
        if (printer_state::get_state(false) == DeviceState::Printing) {
            marlin_client::print_pause();
            out.next = StatusPage(Status::NoContent, parser);
        } else {
            out.next = StatusPage(Status::Conflict, parser);
        }
    }

    void resume_print(const RequestParser &parser, handler::Step &out) {
        if (printer_state::get_state(false) == DeviceState::Paused) {
            marlin_client::print_resume();
            out.next = StatusPage(Status::NoContent, parser);
        } else {
            out.next = StatusPage(Status::Conflict, parser);
        }
    }

    void handle_command(string_view command, const RequestParser &parser, handler::Step &out) {
        if (command == "resume") {
            resume_print(parser, out);
        } else if (command == "pause") {
            pause_print(parser, out);
        } else {
            out.next = StatusPage(Status::BadRequest, parser);
        }
    }
} // namespace

Selector::Accepted PrusaLinkApiV1::accept(const RequestParser &parser, handler::Step &out) const {
    // This is a little bit of a hack (similar one is in Connect). We want to
    // watch as often as possible if the USB is plugged in or not, to
    // invalidate dir listing caches in the browser. As we don't really have a
    // better place, we place it here.
    ChangedPath::instance.media_inserted(wui_media_inserted());

    const string_view uri = parser.uri();

    // Claim the whole /api/v1 prefix.
    const auto suffix_opt = remove_prefix(uri, "/api/v1/");
    if (!suffix_opt.has_value()) {
        return Accepted::NextSelector;
    }

    const auto suffix = *suffix_opt;

    if (!parser.check_auth(out)) {
        return Accepted::Accepted;
    }

    if (suffix == "storage") {
        get_only(SendJson(EmptyRenderer(get_storage), parser.can_keep_alive()), parser, out);
        return Accepted::Accepted;
    } else if (suffix == "info") {
        get_only(SendJson(EmptyRenderer(get_info), parser.can_keep_alive()), parser, out);
        return Accepted::Accepted;
    } else if (auto job_suffix_opt = remove_prefix(suffix, "job/"); job_suffix_opt.has_value()) {
        auto job_suffix = *job_suffix_opt;
        auto id = get_job_id(job_suffix);
        if (!id.has_value()) {
            out.next = StatusPage(Status::BadRequest, parser);
            return Accepted::Accepted;
        }
        if (id.value() != marlin_vars().job_id) {
            out.next = StatusPage(Status::NotFound, parser);
            return Accepted::Accepted;
        }
        switch (parser.method) {
        case Method::Put: {
            auto slash = job_suffix.find('/');
            if (slash == string_view::npos) {
                out.next = StatusPage(Status::BadRequest, parser);
            } else {
                auto command_view = string_view(job_suffix.begin() + job_suffix.find('/') + 1, job_suffix.end());
                handle_command(command_view, parser, out);
            }
            return Accepted::Accepted;
        }
        case Method::Delete:
            stop_print(parser, out);
            return Accepted::Accepted;
        default:
            out.next = StatusPage(Status::MethodNotAllowed, parser);
            return Accepted::Accepted;
        }
    } else if (suffix == "job") {
        if (printer_state::has_job()) {
            get_only(SendJson(EmptyRenderer(get_job_v1), parser.can_keep_alive()), parser, out);
            return Accepted::Accepted;
        } else {
            out.next = StatusPage(Status::NoContent, parser);
            return Accepted::Accepted;
        }
    } else if (suffix == "status") {
        auto status = Monitor::instance.status();
        optional<TransferId> id;
        if (status.has_value()) {
            id = status->id;
        } else {
            id = nullopt;
        }
        get_only(SendJson(StatusRenderer(id), parser.can_keep_alive()), parser, out);
        return Accepted::Accepted;
    } else if (auto gcode_suffix_opt = remove_prefix(suffix, "gcode?"); gcode_suffix_opt.has_value()) {
        auto gcode_suffix = *gcode_suffix_opt;

        // The proper way to do this would be a POST, but we would have to implement
        // a full class, and I don't feel like doing that.

        if (marlin_client::is_printing()) {
            out.next = StatusPage(Status::Conflict, parser, "Print in progress");
            return Accepted::Accepted;
        }

        char decoded_gcode[128];
        const char* buf = gcode_suffix.begin();
        const char* end = gcode_suffix.end();

        while (true) {
            const char* amp = static_cast<const char *>(memchr(buf, '&', end - buf));
            if (amp == nullptr) {
                amp = end;
            }

            string_view raw_gcode(buf, amp - buf);

            if (!url_decode(raw_gcode, decoded_gcode, sizeof(decoded_gcode))) {
                out.next = StatusPage(Status::BadRequest, parser, "url_decode() failed");
                return Accepted::Accepted;
            }

            auto res = marlin_client::gcode_try(decoded_gcode);
            switch(res) {
                case marlin_client::GcodeTryResult::Submitted:
                    break;
                case marlin_client::GcodeTryResult::QueueFull:
                    out.next = StatusPage(Status::TooManyRequests, parser, "Queue full");
                    return Accepted::Accepted;
                case marlin_client::GcodeTryResult::GcodeTooLong:
                    out.next = StatusPage(Status::UriTooLong, parser, "GCode too long");
                    return Accepted::Accepted;
                default:
                    out.next = StatusPage(Status::InternalServerError, parser);
                    return Accepted::Accepted;
            }

            if (amp >= end)
                break;

            buf = amp + 1;
        }

        out.next = StatusPage(Status::NoContent, parser, "Command submitted");
        return Accepted::Accepted;

    } else if (auto gui_suffix_opt = remove_prefix(suffix, "gui?c="); gui_suffix_opt.has_value()) {
        auto gui_suffix = *gui_suffix_opt;
        if (gui_suffix == "press") {
            gui_fake_input(GuiFakeEvent::KnobClick);
        } else if (gui_suffix == "left") {
            gui_fake_encoder(-1);
        } else if (gui_suffix == "right") {
            gui_fake_encoder(1);
        } else if (gui_suffix_opt = remove_prefix(gui_suffix, "tap&p="); gui_suffix_opt.has_value()) {
            point_ui16_t pos;
            gui_suffix = *gui_suffix_opt;
            const char* comma = static_cast<const char *>(memchr(gui_suffix.begin(), ',', gui_suffix.size()));
            if (comma != nullptr) {
                std::from_chars(gui_suffix.begin(), comma, pos.x);
                std::from_chars(comma + 1, gui_suffix.end(), pos.y);
                gui_fake_tap(pos);
            } else {
                out.next = StatusPage(Status::BadRequest, parser, "Invalid position");
                return Accepted::Accepted;
            }

        } else if (gui_suffix_opt = remove_prefix(gui_suffix, "knob&v="); gui_suffix_opt.has_value()) {
            int32_t amt;
            gui_suffix = *gui_suffix_opt;
            std::from_chars(gui_suffix.begin(), gui_suffix.end(), amt);
            gui_fake_encoder(amt);
        } else {
            out.next = StatusPage(Status::NotFound, parser);
            return Accepted::Accepted;
        }
        out.next = StatusPage(Status::NoContent, parser);
        return Accepted::Accepted;
    } else if (suffix == "gui/windows") {
        out.next = GUIText(parser.can_keep_alive(), false, false, true);
        return Accepted::Accepted;
    } else if (suffix == "gui/allwindows") {
        out.next = GUIText(parser.can_keep_alive(), false, false, false);
        return Accepted::Accepted;
    } else if (suffix == "gui/text") {
        out.next = GUIText(parser.can_keep_alive(), true, false, true);
        return Accepted::Accepted;
    } else if (suffix == "gui/alltext") {
        out.next = GUIText(parser.can_keep_alive(), true, false, false);
        return Accepted::Accepted;
    } else if (auto frame_suffix_opt = remove_prefix(suffix, "gui/lcd.qoi?"); frame_suffix_opt.has_value()) {
        auto frame_suffix = *frame_suffix_opt;
        int flags = 0;
        std::from_chars(frame_suffix.begin(), frame_suffix.end(), flags);
        out.next = FrameDump(parser.can_keep_alive(), flags);
        return Accepted::Accepted;
    } else if (suffix == "gui/lcd.qoi") {
        out.next = FrameDump(parser.can_keep_alive());
        return Accepted::Accepted;
    } else if (suffix == "transfer") {
        if (auto status = Monitor::instance.status(); status.has_value()) {
            get_only(SendJson(TransferRenderer(status->id, http::APIVersion::v1), parser.can_keep_alive()), parser, out);
            return Accepted::Accepted;
        } else {
            out.next = StatusPage(Status::NoContent, parser);
            return Accepted::Accepted;
        }
    } else if (remove_prefix(suffix, "files").has_value()) {
        static const auto prefix = "/api/v1/files";
        static const size_t prefix_len = strlen(prefix);
        // We need both one SFN + one LFN in case of upload of a file.
        char filename[FILE_PATH_BUFFER_LEN + FILE_NAME_BUFFER_LEN + prefix_len];
        if (!parse_file_url(parser, prefix_len, filename, sizeof(filename), RemapPolicy::NoRemap, out)) {
            return Accepted::Accepted;
        }
        uint32_t etag = ChangedPath::instance.change_chain_hash(filename);
        if (etag == parser.if_none_match && etag != 0 /* 0 is special */ && (parser.method == Method::Get || parser.method == Method::Head)) {
            out.next = StatusPage(Status::NotModified, parser.status_page_handling(), parser.accepts_json, etag);
            return Accepted::Accepted;
        }
        switch (parser.method) {
        case Method::Put: {
            if (parser.create_folder) {
                out.next = create_folder(filename, parser);
                return Accepted::Accepted;
            } else {
                GcodeUpload::PutParams putParams;
                putParams.overwrite = parser.overwrite_file;
                putParams.print_after_upload = parser.print_after_upload;
                strlcpy(putParams.filepath.data(), filename, sizeof(putParams.filepath));
                auto upload = GcodeUpload::start(parser, wui_uploaded_gcode, parser.accepts_json, std::move(putParams));
                std::visit([&](auto upload) { out.next = std::move(upload); }, std::move(upload));
                return Accepted::Accepted;
            }
        }
        case Method::Get: {
            out.next = FileInfo(filename, parser.can_keep_alive(), parser.accepts_json, false, FileInfo::ReqMethod::Get, FileInfo::APIVersion::v1, etag);
            return Accepted::Accepted;
        }
        case Method::Head: {
            out.next = FileInfo(filename, parser.can_keep_alive(), parser.accepts_json, false, FileInfo::ReqMethod::Head, FileInfo::APIVersion::v1, etag);
            return Accepted::Accepted;
        }
        case Method::Delete: {
            out.next = delete_file(filename, parser);
            return Accepted::Accepted;
        }
        case Method::Post: {
            out.next = print_file(filename, parser);
            return Accepted::Accepted;
        }
        default:
            out.next = StatusPage(Status::MethodNotAllowed, StatusPage::CloseHandling::ErrorClose, parser.accepts_json);
            return Accepted::Accepted;
        }
    } else {
        out.next = StatusPage(Status::NotFound, parser);
        return Accepted::Accepted;
    }
}

const PrusaLinkApiV1 prusa_link_api_v1;

} // namespace nhttp::link_content
