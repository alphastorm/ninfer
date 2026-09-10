// A stop that arrives before the server listens is not lost: listen() returns at once without
// serving, and the request reports that no accept loop was running so a watcher re-asserts it.
#include "serve/http_server.h"

#include <iostream>
#include <stdexcept>

namespace {

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;
    ninfer::serve::HttpServer server{ninfer::serve::ServeOptions{}};
    failures += check(!server.stop(), "stop before listen reported a running accept loop");
    failures += check(!server.stop(), "a repeated stop request reported a running accept loop");
    // No service is attached and no socket is bound: without the remembered request listen()
    // would refuse to run at all, so returning is the proof that the request was honoured.
    bool returned = false;
    try {
        returned = server.listen();
    } catch (const std::logic_error&) {}
    failures += check(returned, "listen after a stop request did not return without serving");
    if (failures == 0) { std::cout << "http server stop tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
