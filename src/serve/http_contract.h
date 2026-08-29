#pragma once

#include "serve/serve_options.h"

#include <httplib.h>

#include <optional>
#include <string>

namespace ninfer::serve {

struct GenerationRequest;

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

// Bodyless stored-response routes carry exactly one optional authenticated session digest.
std::optional<std::string> response_session_identity(const httplib::Request& request,
                                                     bool authentication_configured);
// Checkpoint status and namespace deletion require exactly one authenticated session header.
std::string require_checkpoint_session_header(const httplib::Request& request,
                                              bool authentication_configured);
void apply_response_session_identity(const httplib::Request& request,
                                     bool authentication_configured,
                                     GenerationRequest& generation);

} // namespace ninfer::serve
