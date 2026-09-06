#include "stdafx.h"
#include "dgate_service.h"
#include "geoip_router.h"
#include "master_service.h"
#include "dns_http_service.h"

#include <cctype>
#include <string>
#include <thread>

namespace {

bool normalize_domain(const char* value, acl::string& domain)
{
	domain = value == NULL ? "" : value;
	domain.trim_space();
	domain.lower();
	while (!domain.empty() && domain[domain.size() - 1] == '.') {
		domain.pop_back();
	}
	if (domain.empty() || domain.size() > 253) {
		return false;
	}

	size_t label_length = 0;
	for (size_t i = 0; i < domain.size(); ++i) {
		unsigned char ch = static_cast<unsigned char>(domain[i]);
		if (ch == '.') {
			if (label_length == 0 || label_length > 63 || domain[i - 1] == '-') {
				return false;
			}
			label_length = 0;
			continue;
		}
		if ((!std::isalnum(ch) && ch != '-')
			|| (label_length == 0 && ch == '-')) {
			return false;
		}
		++label_length;
	}
	return label_length > 0 && label_length <= 63
		&& domain[domain.size() - 1] != '-';
}

bool write_error(acl::HttpServletResponse& response, int status,
	const char* message)
{
	response.setStatus(status);
	response.setContentType("application/json; charset=utf-8");
	acl::json json;
	json.get_root().add_bool("success", false).add_text("message", message);
	return response.write(json);
}

bool handle_domain_lookup(acl::HttpServletRequest& request,
	acl::HttpServletResponse& response)
{
	acl::string domain;
	if (!normalize_domain(request.getParameter("domain"), domain)) {
		return write_error(response, 400, "parameter 'domain' is invalid");
	}

	acl::rfc1035_response dns_response;
	acl::string error;
	if (!dgate_resolve_domain(domain.c_str(), dns_response, error)) {
		logger_error("HTTP DNS lookup failed, domain=%s, error=%s",
			domain.c_str(), error.c_str());
		return write_error(response, 502, error.c_str());
	}

	acl::json json;
	acl::json_node& root = json.get_root();
	acl::json_node& addresses = json.create_node(true);
	long long matched_count = 0;
	const std::vector<acl::string>& values = dns_response.get_addrs4a();
	for (std::vector<acl::string>::const_iterator it = values.begin();
		it != values.end(); ++it) {
		std::string country;
		bool route_matched = false;
		bool country_found = geoip_lookup_address(it->c_str(), country,
			route_matched);
		if (route_matched) {
			++matched_count;
		}
		addresses.add_child(json.create_node()
			.add_text("ip", it->c_str())
			.add_bool("country_found", country_found)
			.add_text("country", country.c_str())
			.add_bool("route_matched", route_matched));
	}

	acl::json_node& cnames = json.create_node(true);
	const std::vector<acl::string>& aliases = dns_response.get_cnames();
	for (std::vector<acl::string>::const_iterator it = aliases.begin();
		it != aliases.end(); ++it) {
		cnames.add_child(json.create_node().add_text("name", it->c_str()));
	}

	response.setStatus(200);
	response.setContentType("application/json; charset=utf-8");
	root.add_bool("success", true)
		.add_text("domain", domain.c_str())
		.add_text("upstream", var_cfg_upstream_addr)
		.add_bool("database_ready", geoip_database_ready())
		.add_bool("routing_ready", geoip_routing_ready())
		.add_number("count", static_cast<long long>(values.size()))
		.add_number("matched_count", matched_count)
		.add_bool("would_route", matched_count > 0 && geoip_routing_ready())
		.add_child("cnames", cnames)
		.add_child("addresses", addresses);
	logger("HTTP DNS lookup completed, domain=%s, addresses=%lu, matched=%lld",
		domain.c_str(), static_cast<unsigned long>(values.size()), matched_count);
	return response.write(json);
}

class dns_http_servlet : public acl::HttpServlet {
public:
	explicit dns_http_servlet(acl::socket_stream* connection)
	: acl::HttpServlet(connection) {}
	~dns_http_servlet(void) {}

protected:
	bool doGet(acl::HttpServletRequest& request,
		acl::HttpServletResponse& response)
	{
		response.setKeepAlive(request.isKeepAlive());
		const char* path = request.getPathInfo();
		if (path != NULL && (strcmp(path, "/lookup") == 0
			|| strcmp(path, "/lookup/") == 0)) {
			return handle_domain_lookup(request, response);
		}
		if (path != NULL && (strcmp(path, "/health") == 0
			|| strcmp(path, "/health/") == 0)) {
			response.setStatus(200);
			response.setContentType("application/json; charset=utf-8");
			acl::json json;
			json.get_root().add_bool("success", true)
				.add_bool("database_ready", geoip_database_ready())
				.add_bool("routing_ready", geoip_routing_ready());
			return response.write(json);
		}
		return write_error(response, 404, "not found");
	}
};

void serve_http_client(acl::socket_stream* connection)
{
	connection->set_rw_timeout(30);
	dns_http_servlet servlet(connection);
	servlet.setLocalCharset("utf-8");
	while (servlet.doRun()) {}
	delete connection;
}

void run_http_server(const std::string& address)
{
	acl::server_socket server(acl::OPEN_FLAG_REUSEPORT, 128);
	if (!server.open(address.c_str())) {
		logger_error("start dns-gate HTTP service failed, addr=%s, error=%s",
			address.c_str(), acl::last_serror());
		return;
	}
	logger("dns-gate HTTP service listening on %s", address.c_str());
	for (;;) {
		acl::socket_stream* connection = server.accept();
		if (connection == NULL) {
			logger_error("dns-gate HTTP accept failed, error=%s",
				acl::last_serror());
			break;
		}
		go[=] { serve_http_client(connection); };
	}
}

} // namespace

void dns_http_service_start(const char* address)
{
	if (address == NULL || *address == 0) {
		logger_warn("dns-gate HTTP service disabled: http_addr is empty");
		return;
	}
	std::string value(address);
	std::thread worker([value] {
		go[value] { run_http_server(value); };
		acl::fiber::schedule();
	});
	worker.detach();
}
