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
        auto result = std::from_chars(str.begin(), str.end(), ret);
        if (result.ec != std::errc {}) {
            return nullopt;
        }

        return ret;
    }

    ConnectionState stop_print(const RequestParser &parser) {
        switch (printer_state::get_state(false)) {
        case DeviceState::Printing:
        case DeviceState::Paused:
        case DeviceState::Attention:
            marlin_client::print_abort();
            return StatusPage(Status::NoContent, parser);
        default:
            return StatusPage(Status::Conflict, parser);
        }
    }

    ConnectionState pause_print(const RequestParser &parser) {
        if (printer_state::get_state(false) == DeviceState::Printing) {
            marlin_client::print_pause();
            return StatusPage(Status::NoContent, parser);
        } else {
            return StatusPage(Status::Conflict, parser);
        }
    }

    ConnectionState resume_print(const RequestParser &parser) {
        if (printer_state::get_state(false) == DeviceState::Paused) {
            marlin_client::print_resume();
            return StatusPage(Status::NoContent, parser);
        } else {
            return StatusPage(Status::Conflict, parser);
        }
    }

    ConnectionState handle_command(string_view command, const RequestParser &parser) {
        if (command == "resume") {
            return resume_print(parser);
        } else if (command == "pause") {
            return pause_print(parser);
        } else {
            return StatusPage(Status::BadRequest, parser);
        }
    }
} // namespace

optional<ConnectionState> PrusaLinkApiV1::accept(const RequestParser &parser) const {
    // This is a little bit of a hack (similar one is in Connect). We want to
    // watch as often as possible if the USB is plugged in or not, to
    // invalidate dir listing caches in the browser. As we don't really have a
    // better place, we place it here.
    ChangedPath::instance.media_inserted(wui_media_inserted());

    const string_view uri = parser.uri();

    // Claim the whole /api/v1 prefix.
    const auto suffix_opt = remove_prefix(uri, "/api/v1/");
    if (!suffix_opt.has_value()) {
        return nullopt;
    }

    const auto suffix = *suffix_opt;

    if (auto unauthorized_status = parser.authenticated_status(); unauthorized_status.has_value()) {
        return std::visit([](auto unauth_status) -> ConnectionState { return unauth_status; }, *unauthorized_status);
    }

    if (suffix == "storage") {
        return get_only(SendJson(EmptyRenderer(get_storage), parser.can_keep_alive()), parser);
    } else if (suffix == "info") {
        return get_only(SendJson(EmptyRenderer(get_info), parser.can_keep_alive()), parser);
    } else if (auto job_suffix_opt = remove_prefix(suffix, "job/"); job_suffix_opt.has_value()) {
        auto job_suffix = *job_suffix_opt;
        auto id = get_job_id(job_suffix);
        if (!id.has_value()) {
            return StatusPage(Status::BadRequest, parser);
        }
        if (id.value() != marlin_vars()->job_id) {
            return StatusPage(Status::NotFound, parser);
        }
        switch (parser.method) {
        case Method::Put: {
            auto slash = job_suffix.find('/');
            if (slash == string_view::npos) {
                return StatusPage(Status::BadRequest, parser);
            }
            auto command_view = string_view(job_suffix.begin() + job_suffix.find('/') + 1, job_suffix.end());
            return handle_command(command_view, parser);
        }
        case Method::Delete:
            return stop_print(parser);
        default:
            return StatusPage(Status::MethodNotAllowed, parser);
        }
    } else if (suffix == "job") {
        if (printer_state::has_job()) {
            return get_only(SendJson(EmptyRenderer(get_job_v1), parser.can_keep_alive()), parser);
        } else {
            return StatusPage(Status::NoContent, parser);
        }
    } else if (suffix == "status") {
        auto status = Monitor::instance.status();
        optional<TransferId> id;
        if (status.has_value()) {
            id = status->id;
        } else {
            id = nullopt;
        }
        return get_only(SendJson(StatusRenderer(id), parser.can_keep_alive()), parser);
    } else if (auto gcode_suffix_opt = remove_prefix(suffix, "gcode?"); gcode_suffix_opt.has_value()) {
        auto gcode_suffix = *gcode_suffix_opt;

        // The proper way to do this would be a POST, but we would have to implement
        // a full class, and I don't feel like doing that.

        if (marlin_client::is_printing()) {
            return StatusPage(Status::Conflict, parser, "Print in progress");
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
                return StatusPage(Status::BadRequest, parser, "url_decode() failed");
            }

            auto res = marlin_client::gcode_try(decoded_gcode);
            switch(res) {
                case marlin_client::GcodeTryResult::Submitted:
                    break;
                case marlin_client::GcodeTryResult::QueueFull:
                    return StatusPage(Status::TooManyRequests, parser, "Queue full");
                case marlin_client::GcodeTryResult::GcodeTooLong:
                    return StatusPage(Status::UriTooLong, parser, "GCode too long");
                default:
                    return StatusPage(Status::InternalServerError, parser);
            }

            if (amp >= end)
                break;

            buf = amp + 1;
        }

        return StatusPage(Status::NoContent, parser, "Command submitted");

    } else if (auto gui_suffix_opt = remove_prefix(suffix, "gui?c="); gui_suffix_opt.has_value()) {
        auto gui_suffix = *gui_suffix_opt;
        if (gui_suffix == "press") {
            gui_fake_input(GuiFakeEvent::KnobClick);
        } else if (gui_suffix == "left") {
            gui_fake_input(GuiFakeEvent::KnobLeft);
        } else if (gui_suffix == "right") {
            gui_fake_input(GuiFakeEvent::KnobRight);
        } else if (gui_suffix_opt = remove_prefix(gui_suffix, "tap&p="); gui_suffix_opt.has_value()) {
            point_ui16_t pos;
            gui_suffix = *gui_suffix_opt;
            const char* comma = static_cast<const char *>(memchr(gui_suffix.begin(), ',', gui_suffix.size()));
            if (comma != nullptr) {
                std::from_chars(gui_suffix.begin(), comma, pos.x);
                std::from_chars(comma + 1, gui_suffix.end(), pos.y);
                gui_fake_tap(pos);
            } else {
                return StatusPage(Status::BadRequest, parser, "Invalid position");
            }

        } else {
            return StatusPage(Status::NotFound, parser);
        }
        return StatusPage(Status::NoContent, parser);
    } else if (suffix == "gui/windows") {
        return GUIText(parser.can_keep_alive(), false, false, true);
    } else if (suffix == "gui/allwindows") {
        return GUIText(parser.can_keep_alive(), false, false, false);
    } else if (suffix == "gui/text") {
        return GUIText(parser.can_keep_alive(), true, false, true);
    } else if (suffix == "gui/alltext") {
        return GUIText(parser.can_keep_alive(), true, false, false);
    } else if (auto frame_suffix_opt = remove_prefix(suffix, "gui/lcd.qoi?"); frame_suffix_opt.has_value()) {
        auto frame_suffix = *frame_suffix_opt;
        int flags = 0;
        std::from_chars(frame_suffix.begin(), frame_suffix.end(), flags);
        return FrameDump(parser.can_keep_alive(), flags);
    } else if (suffix == "gui/lcd.qoi") {
        return FrameDump(parser.can_keep_alive());
    } else if (suffix == "transfer") {
        if (auto status = Monitor::instance.status(); status.has_value()) {
            return get_only(SendJson(TransferRenderer(status->id, http::APIVersion::v1), parser.can_keep_alive()), parser);
        } else {
            return StatusPage(Status::NoContent, parser);
        }
    } else if (remove_prefix(suffix, "files").has_value()) {
        static const auto prefix = "/api/v1/files";
        static const size_t prefix_len = strlen(prefix);
        // We need both one SFN + one LFN in case of upload of a file.
        char filename[FILE_PATH_BUFFER_LEN + FILE_NAME_BUFFER_LEN + prefix_len];
        auto error = parse_file_url(parser, prefix_len, filename, sizeof(filename), RemapPolicy::NoRemap);
        if (error.has_value()) {
            return error;
        }
        uint32_t etag = ChangedPath::instance.change_chain_hash(filename);
        if (etag == parser.if_none_match && etag != 0 /* 0 is special */ && (parser.method == Method::Get || parser.method == Method::Head)) {
            return StatusPage(Status::NotModified, parser.status_page_handling(), parser.accepts_json, etag);
        }
        switch (parser.method) {
        case Method::Put: {
            if (parser.create_folder) {
                return create_folder(filename, parser);
            } else {
                GcodeUpload::PutParams putParams;
                putParams.overwrite = parser.overwrite_file;
                putParams.print_after_upload = parser.print_after_upload;
                strlcpy(putParams.filepath.data(), filename, sizeof(putParams.filepath));
                auto upload = GcodeUpload::start(parser, wui_uploaded_gcode, parser.accepts_json, std::move(putParams));
                return std::visit([](auto upload) -> ConnectionState { return upload; }, std::move(upload));
            }
        }
        case Method::Get: {
            return FileInfo(filename, parser.can_keep_alive(), parser.accepts_json, false, FileInfo::ReqMethod::Get, FileInfo::APIVersion::v1, etag);
        }
        case Method::Head: {
            return FileInfo(filename, parser.can_keep_alive(), parser.accepts_json, false, FileInfo::ReqMethod::Head, FileInfo::APIVersion::v1, etag);
        }
        case Method::Delete: {
            return delete_file(filename, parser);
        }
        case Method::Post: {
            return print_file(filename, parser);
        }
        default:
            return StatusPage(Status::MethodNotAllowed, StatusPage::CloseHandling::ErrorClose, parser.accepts_json);
        }
    } else {
        return StatusPage(Status::NotFound, parser);
    }
}

const PrusaLinkApiV1 prusa_link_api_v1;

} // namespace nhttp::link_content
