#include "serve/operational_log.h"
#include "product/logging/pretty_format.h"

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace ninfer::serve;
    try {
        const RequestLogContext context{.id = 7, .protocol = "openai-chat"};
        RequestFailure failure{.classification = RequestFailureClass::Internal,
                               .http_status = 500,
                               .machine_message = "sentinel-internal-detail\nsecond line"};
        const auto detail = render_request_failure(context, failure);
        const auto response = render_response_failure(7, failure);
        failure.machine_message.clear();
        const auto empty = render_request_failure(context, failure);
        const auto expected = empty.message + " | " +
            ninfer::product::format_pretty_text("sentinel-internal-detail\nsecond line");
        if (detail.severity != OperationalSeverity::Error || detail.message != expected ||
            detail.message.find('\n') != std::string::npos ||
            response.message.find("sentinel-internal-detail") == std::string::npos) {
            throw std::runtime_error("internal failure detail is missing or not single-line");
        }
        for (const auto classification : {RequestFailureClass::ClientInput,
                                         RequestFailureClass::Timeout,
                                         RequestFailureClass::Unavailable}) {
            failure.classification = classification;
            const auto before = render_request_failure(context, failure);
            failure.machine_message = "private-client-detail";
            const auto after = render_request_failure(context, failure);
            if (before.message != after.message)
                throw std::runtime_error("non-internal failure exposed its machine detail");
            failure.machine_message.clear();
        }
        std::cout << "ok\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
