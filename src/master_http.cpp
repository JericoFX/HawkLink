#include "hawken/master_http.hpp"
#include "hawken/meteor_compat.hpp"
#include <cpp-httplib/httplib.h>

namespace hawken::master {
void configure_master_http(httplib::Server& server, const MasterDirectory& directory) {
    server.set_read_timeout(2);
    server.set_write_timeout(2);
    server.set_payload_max_length(8192);
    server.set_exception_handler([](const httplib::Request&, httplib::Response& response, std::exception_ptr) {
        response.status = 500;
        response.set_content(meteor::envelope(500, "Request could not be processed.", nullptr).dump(),
            "application/json; charset=utf-8");
    });
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        response.set_content(meteor::envelope(static_cast<std::uint32_t>(response.status),
            "Request could not be processed.", nullptr).dump(), "application/json; charset=utf-8");
    });
    server.set_pre_routing_handler([&directory](const httplib::Request& request, httplib::Response& response) {
        const auto result = directory.respond(request.method, request.target);
        response.status = result.http_status;
        if (response.status == 405) response.set_header("Allow", "GET");
        response.set_content(result.body.dump(), "application/json; charset=utf-8");
        return httplib::Server::HandlerResponse::Handled;
    });
}
}
