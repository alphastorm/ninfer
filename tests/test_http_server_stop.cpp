#include "serve/http_server.h"

#include <iostream>
#include <stdexcept>

int main() {
    ninfer::serve::HttpServer server{ninfer::serve::ServeOptions{}};
    if (server.stop() || server.stop()) {
        std::cerr << "stop before listen reported a running accept loop\n";
        return 1;
    }
    // No attached service/socket: returning proves the remembered request won over startup.
    try {
        if (!server.listen()) {
            std::cerr << "listen did not honour the remembered stop\n";
            return 1;
        }
    } catch (const std::logic_error&) {
        std::cerr << "stop during model loading was lost\n";
        return 1;
    }
    std::cout << "http server stop tests passed\n";
    return 0;
}
