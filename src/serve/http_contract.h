#pragma once

#include "serve/serve_options.h"

#include <httplib.h>

namespace ninfer::serve {

// cpp-httplib invokes the error handler for every application response with status >= 400. Only
// an empty 413 is its own pre-routing payload-limit rejection; application-authored errors must be
// left untouched.
httplib::Server::HandlerResponse handle_unrendered_http_error(const ServeOptions& options,
                                                              const httplib::Request& request,
                                                              httplib::Response& response);

// Shared pre-routing authentication contract, public for host-side protocol tests.
httplib::Server::HandlerResponse authorize_http_request(const ServeOptions& options,
                                                        const httplib::Request& request,
                                                        httplib::Response& response);

} // namespace ninfer::serve
