#pragma once
#include "shared/protocol.h"
#include "xkit/xbus_client.h"
#include <functional>
#include <string>
#include <vector>

namespace fbx::xkit {

// XPrint — client-side helper for the com.freebsdx.Print portal.
//
// All methods are asynchronous — results arrive via callbacks on the
// XBusClient event loop.
//
class XPrint {
public:
    using PrinterListCallback = std::function<void(
        const std::vector<shared::PrinterInfo>&, bool /*done*/)>;
    using JobCallback = std::function<void(
        bool /*success*/, uint32_t /*job_id*/, const std::string& /*message*/)>;
    using StatusCallback = std::function<void(
        const shared::PrintJobStatus&)>;

    // List available CUPS printers.
    //
    // Sends a ListPrinters request for `page` (0-based). The callback receives
    // a vector containing the single printer on that page (empty vector if past
    // end), plus `done=true` when page >= total-1.
    //
    // To iterate all printers, call listPrinters repeatedly with page=0,1,2,…
    // until done==true is delivered.
    static void listPrinters(XBusClient& bus,
                             uint32_t page,
                             PrinterListCallback cb) noexcept;

    // Convenience overload: page=0.
    static void listPrinters(XBusClient& bus,
                             PrinterListCallback cb) noexcept;

    // Print a file. `printer_name` empty = system default.
    static void printFile(XBusClient& bus,
                          const std::string& file_path,
                          const std::string& title,
                          const std::string& printer_name = "",
                          uint32_t copies = 1,
                          uint32_t color_mode = 0,
                          uint32_t duplex = 0,
                          uint32_t paper_size = 0,
                          JobCallback cb = {}) noexcept;

    // Portal convenience: let the compositor pick the default printer.
    static void showPrintDialog(XBusClient& bus,
                                const std::string& file_path,
                                const std::string& title,
                                JobCallback cb = {}) noexcept;

    // Poll job status.
    static void getJobStatus(XBusClient& bus,
                             uint32_t job_id,
                             const std::string& printer_name,
                             StatusCallback cb) noexcept;

    // Cancel a print job.
    static void cancelJob(XBusClient& bus,
                          uint32_t job_id,
                          const std::string& printer_name,
                          JobCallback cb = {}) noexcept;
};

} // namespace fbx::xkit
