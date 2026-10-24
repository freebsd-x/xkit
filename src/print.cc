#include "xkit/print.h"
#include <cstring>
#include <span>

namespace fbx::xkit {

// ─── ListPrinters ─────────────────────────────────────────────────────────────

void XPrint::listPrinters(XBusClient& bus,
                           uint32_t page,
                           PrinterListCallback cb) noexcept {
    // Payload is a single uint32_t: the 0-based page number.
    uint32_t page_payload = page;
    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(&page_payload), sizeof(page_payload));

    bus.Call("com.freebsdx.Print",
             static_cast<uint32_t>(shared::PrintOpcode::ListPrinters),
             payload,
             [cb = std::move(cb)](const shared::XBusMessage& msg) {
                 if (!cb) return;
                 shared::PrinterListPage pl{};
                 std::memcpy(&pl, msg.payload_, sizeof(pl));

                 std::vector<shared::PrinterInfo> result;
                 if (pl.count_ > 0) {
                     result.push_back(pl.printer_);
                 }

                 // done when this page is the last one (page_ >= total_-1),
                 // or when total_ == 0, or when count_ == 0.
                 bool done = (pl.total_ == 0) ||
                             (pl.count_ == 0) ||
                             (pl.page_ + 1 >= pl.total_);
                 cb(result, done);
             });
}

void XPrint::listPrinters(XBusClient& bus,
                           PrinterListCallback cb) noexcept {
    listPrinters(bus, 0u, std::move(cb));
}

// ─── PrintFile ────────────────────────────────────────────────────────────────

void XPrint::printFile(XBusClient& bus,
                        const std::string& file_path,
                        const std::string& title,
                        const std::string& printer_name,
                        uint32_t copies,
                        uint32_t color_mode,
                        uint32_t duplex,
                        uint32_t paper_size,
                        JobCallback cb) noexcept {
    shared::PrintRequest req{};
    std::strncpy(req.printer_name_, printer_name.c_str(), sizeof(req.printer_name_) - 1);
    std::strncpy(req.file_path_,    file_path.c_str(),    sizeof(req.file_path_) - 1);
    std::strncpy(req.title_,        title.c_str(),        sizeof(req.title_) - 1);
    req.copies_     = (copies >= 1) ? copies : 1u;
    req.color_mode_ = color_mode;
    req.duplex_     = duplex;
    req.paper_size_ = paper_size;

    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(&req), sizeof(req));

    bus.Call("com.freebsdx.Print",
             static_cast<uint32_t>(shared::PrintOpcode::PrintFile),
             payload,
             [cb = std::move(cb)](const shared::XBusMessage& msg) {
                 if (!cb) return;
                 shared::PrintResponse resp{};
                 std::memcpy(&resp, msg.payload_, sizeof(resp));
                 std::string message(resp.message_,
                     strnlen(resp.message_, sizeof(resp.message_)));
                 cb(resp.success_ != 0, resp.job_id_, message);
             });
}

// ─── ShowPrintDialog ──────────────────────────────────────────────────────────

void XPrint::showPrintDialog(XBusClient& bus,
                              const std::string& file_path,
                              const std::string& title,
                              JobCallback cb) noexcept {
    shared::PrintDialogRequest dreq{};
    std::strncpy(dreq.file_path_, file_path.c_str(), sizeof(dreq.file_path_) - 1);
    std::strncpy(dreq.title_,     title.c_str(),     sizeof(dreq.title_) - 1);

    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(&dreq), sizeof(dreq));

    bus.Call("com.freebsdx.Print",
             static_cast<uint32_t>(shared::PrintOpcode::ShowDialog),
             payload,
             [cb = std::move(cb)](const shared::XBusMessage& msg) {
                 if (!cb) return;
                 shared::PrintResponse resp{};
                 std::memcpy(&resp, msg.payload_, sizeof(resp));
                 std::string message(resp.message_,
                     strnlen(resp.message_, sizeof(resp.message_)));
                 cb(resp.success_ != 0, resp.job_id_, message);
             });
}

// ─── GetJobStatus ─────────────────────────────────────────────────────────────

void XPrint::getJobStatus(XBusClient& bus,
                           uint32_t job_id,
                           const std::string& printer_name,
                           StatusCallback cb) noexcept {
    // Payload: uint32_t job_id + char printer_name[48]
    struct QueryPayload {
        uint32_t job_id_;
        char     printer_name_[48];
    } q{};
    static_assert(sizeof(QueryPayload) == 52);

    q.job_id_ = job_id;
    std::strncpy(q.printer_name_, printer_name.c_str(), sizeof(q.printer_name_) - 1);

    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(&q), sizeof(q));

    bus.Call("com.freebsdx.Print",
             static_cast<uint32_t>(shared::PrintOpcode::GetJobStatus),
             payload,
             [cb = std::move(cb)](const shared::XBusMessage& msg) {
                 if (!cb) return;
                 shared::PrintJobStatus status{};
                 std::memcpy(&status, msg.payload_, sizeof(status));
                 cb(status);
             });
}

// ─── CancelJob ────────────────────────────────────────────────────────────────

void XPrint::cancelJob(XBusClient& bus,
                        uint32_t job_id,
                        const std::string& printer_name,
                        JobCallback cb) noexcept {
    // Same payload layout as GetJobStatus
    struct QueryPayload {
        uint32_t job_id_;
        char     printer_name_[48];
    } q{};

    q.job_id_ = job_id;
    std::strncpy(q.printer_name_, printer_name.c_str(), sizeof(q.printer_name_) - 1);

    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(&q), sizeof(q));

    bus.Call("com.freebsdx.Print",
             static_cast<uint32_t>(shared::PrintOpcode::CancelJob),
             payload,
             [cb = std::move(cb)](const shared::XBusMessage& msg) {
                 if (!cb) return;
                 shared::PrintResponse resp{};
                 std::memcpy(&resp, msg.payload_, sizeof(resp));
                 std::string message(resp.message_,
                     strnlen(resp.message_, sizeof(resp.message_)));
                 cb(resp.success_ != 0, resp.job_id_, message);
             });
}

} // namespace fbx::xkit
