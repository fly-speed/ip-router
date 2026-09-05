#include "stdafx.h"
#include "http_service.h"
#include "route_manager.h"
#include "route_service.h"

#include <errno.h>
#include <fstream>
#include <iterator>
#include <set>
#include <stdlib.h>
#include <string>

namespace {

bool reply_json(HttpResponse& response, int status, bool success,
	const char* message, const char* destination = NULL,
	const char* gateway = NULL, const char* key = NULL, long long ttl = -1)
{
	response.setStatus(status);
	response.setContentType("application/json; charset=utf-8");
	acl::json json;
	acl::json_node& root = json.get_root();
	root.add_bool("success", success).add_text("message", message);
	if (destination != NULL) {
		root.add_text("ip", destination);
	}
	if (gateway != NULL) {
		root.add_text("gateway", gateway);
	}
	if (key != NULL) {
		root.add_text("key", key);
	}
	if (ttl >= 0) {
		root.add_number("ttl", ttl);
	}
	return response.write(json);
}

bool valid_route_key(const char* key)
{
	if (key == NULL || *key == 0) {
		return true;
	}
	size_t length = strlen(key);
	if (length > 256) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		unsigned char ch = static_cast<unsigned char>(key[i]);
		if (ch < 0x20 || ch == 0x7f) {
			return false;
		}
	}
	return true;
}

bool parse_ttl(HttpRequest& request, long long& ttl, acl::string& error)
{
	ttl = 0;
	const char* value = request.getParameter("ttl");
	if (value == NULL || *value == 0) {
		return true;
	}

	errno = 0;
	char* end = NULL;
	long long parsed = strtoll(value, &end, 10);
	if (errno != 0 || end == value || *end != 0) {
		error = "parameter 'ttl' must be an integer number of seconds";
		return false;
	}
	ttl = parsed > 0 ? parsed : 0;
	return true;
}

const char* parameter(HttpRequest& request, const char* primary,
	const char* alias)
{
	const char* value = request.getParameter(primary);
	return value != NULL && *value != 0 ? value : request.getParameter(alias);
}

const char* get_route_key(HttpRequest& request)
{
	return parameter(request, "key", "domain");
}

bool change_route(HttpRequest& request, HttpResponse& response, bool add)
{
	const char* destination = parameter(request, "ip", "target");
	const char* gateway = parameter(request, "gateway", "route");
	const char* key = get_route_key(request);
	if (!route_manager::valid_ipv4(destination)) {
		return reply_json(response, 400, false,
			"parameter 'ip' must be a valid IPv4 address");
	}
	if (!route_manager::valid_ipv4(gateway)) {
		return reply_json(response, 400, false,
			"parameter 'gateway' must be a valid IPv4 address");
	}
	if (key == NULL || *key == 0) {
		return reply_json(response, 400, false,
			"parameter 'key' is required");
	}
	if (!valid_route_key(key)) {
		return reply_json(response, 400, false,
			"parameter 'key' is too long or contains control characters");
	}
	long long ttl = 0;
	acl::string ttl_error;
	if (add && !parse_ttl(request, ttl, ttl_error)) {
		return reply_json(response, 400, false, ttl_error.c_str());
	}

	acl::string error;
	bool success = add
		? route_manager::add(destination, gateway, key, ttl, error)
		: route_manager::remove(key, destination, gateway, error);
	if (!success) {
		logger_error("route %s failed, key=%s, ip=%s, gateway=%s, "
			"ttl=%lld, error=%s", add ? "add" : "delete", key,
			destination, gateway, ttl, error.c_str());
		return reply_json(response, 500, false, error.c_str(), destination,
			gateway, key);
	}

	logger("route %s succeeded, key=%s, ip=%s, gateway=%s, ttl=%lld",
		add ? "add" : "delete", key, destination, gateway, ttl);
	return reply_json(response, 200, true,
		add ? "route added" : "route deleted", destination, gateway,
		key, add ? ttl : -1);
}

bool route_add(HttpRequest& request, HttpResponse& response)
{
	const char* values = request.getParameter("ips");
	if (values == NULL || *values == 0) {
		return change_route(request, response, true);
	}

	const char* gateway = parameter(request, "gateway", "route");
	const char* key = get_route_key(request);
	if (!route_manager::valid_ipv4(gateway)) {
		return reply_json(response, 400, false,
			"parameter 'gateway' must be a valid IPv4 address");
	}
	if (key == NULL || *key == 0) {
		return reply_json(response, 400, false,
			"parameter 'key' is required");
	}
	if (!valid_route_key(key)) {
		return reply_json(response, 400, false,
			"parameter 'key' is too long or contains control characters");
	}
	long long ttl = 0;
	acl::string ttl_error;
	if (!parse_ttl(request, ttl, ttl_error)) {
		return reply_json(response, 400, false, ttl_error.c_str());
	}

	acl::string buffer(values);
	const std::vector<acl::string>& tokens = buffer.split2(",; \t\r\n");
	if (tokens.empty() || tokens.size() > 256) {
		return reply_json(response, 400, false,
			"parameter 'ips' must contain between 1 and 256 IPv4 addresses");
	}

	std::vector<acl::string> destinations;
	std::set<acl::string> unique;
	for (std::vector<acl::string>::const_iterator it = tokens.begin();
		it != tokens.end(); ++it) {
		if (!route_manager::valid_ipv4(it->c_str())) {
			acl::string message;
			message.format("invalid IPv4 address in 'ips': %s", it->c_str());
			return reply_json(response, 400, false, message.c_str());
		}
		if (unique.insert(*it).second) {
			destinations.push_back(*it);
		}
	}

	acl::json json;
	acl::json_node& root = json.get_root();
	acl::json_node& results = json.create_node(true);
	size_t succeeded = 0;
	for (std::vector<acl::string>::const_iterator it = destinations.begin();
		it != destinations.end(); ++it) {
		acl::string error;
		bool success = route_manager::add(it->c_str(), gateway, key, ttl, error);
		acl::json_node& item = json.create_node()
			.add_text("ip", it->c_str())
			.add_bool("success", success)
			.add_text("message", success ? "route added" : error.c_str());
		results.add_child(item);
		if (success) {
			++succeeded;
			logger("route add succeeded, key=%s, ip=%s, gateway=%s, "
				"ttl=%lld", key, it->c_str(), gateway, ttl);
		} else {
			logger_error("route add failed, key=%s, ip=%s, gateway=%s, "
				"ttl=%lld, error=%s", key, it->c_str(), gateway, ttl,
				error.c_str());
		}
	}

	bool all_succeeded = succeeded == destinations.size();
	logger("route add completed, key=%s, gateway=%s, ttl=%lld, "
		"requested=%lu, succeeded=%lu", key, gateway, ttl,
		static_cast<unsigned long>(destinations.size()),
		static_cast<unsigned long>(succeeded));
	int status = all_succeeded ? 200 : (succeeded == 0 ? 500 : 207);
	response.setStatus(status);
	response.setContentType("application/json; charset=utf-8");
	root.add_bool("success", all_succeeded)
		.add_text("message", all_succeeded
			? "all routes added" : "one or more routes failed")
		.add_text("gateway", gateway)
		.add_text("key", key ? key : "")
		.add_number("ttl", ttl)
		.add_number("count", static_cast<long long>(destinations.size()))
		.add_number("succeeded", static_cast<long long>(succeeded))
		.add_child("routes", results);
	return response.write(json);
}

bool route_delete(HttpRequest& request, HttpResponse& response)
{
	const char* key = get_route_key(request);
	const char* destination = parameter(request, "ip", "target");
	bool has_key = key != NULL && *key != 0;
	bool has_destination = destination != NULL && *destination != 0;
	if (!has_key && !has_destination) {
		return reply_json(response, 400, false,
			"at least one of 'key' or 'ip' is required");
	}
	if (has_key && !valid_route_key(key)) {
		return reply_json(response, 400, false,
			"parameter 'key' is too long or contains control characters");
	}
	if (has_destination && !route_manager::valid_ipv4(destination)) {
		return reply_json(response, 400, false,
			"parameter 'ip' must be a valid IPv4 address");
	}

	std::vector<route_entry> routes;
	route_manager::list(routes);
	acl::json json;
	acl::json_node& root = json.get_root();
	acl::json_node& results = json.create_node(true);
	size_t matched = 0;
	size_t succeeded = 0;
	std::set<acl::string> deleted_system_routes;
	for (std::vector<route_entry>::const_iterator group = routes.begin();
		group != routes.end(); ++group) {
		if (has_key && group->key != key) {
			continue;
		}
		for (std::vector<route_target>::const_iterator target
			= group->targets.begin(); target != group->targets.end(); ++target) {
			if (has_destination && target->ip != destination) {
				continue;
			}
			++matched;
			acl::string error;
			acl::string route_id;
			route_id.format("%s\n%s", target->ip.c_str(),
				target->gateway.c_str());
			bool already_deleted = deleted_system_routes.find(route_id)
				!= deleted_system_routes.end();
			bool success = already_deleted || route_manager::remove(
				group->key.c_str(), target->ip.c_str(),
				target->gateway.c_str(), error);
			if (success) {
				++succeeded;
				deleted_system_routes.insert(route_id);
				logger("route delete succeeded, key=%s, ip=%s, gateway=%s, "
					"system_already_deleted=%s",
					group->key.c_str(), target->ip.c_str(),
					target->gateway.c_str(), already_deleted ? "yes" : "no");
			} else {
				logger_error("route delete failed, key=%s, ip=%s, "
					"gateway=%s, error=%s", group->key.c_str(),
					target->ip.c_str(), target->gateway.c_str(),
					error.c_str());
			}
			results.add_child(json.create_node()
				.add_text("key", group->key.c_str())
				.add_text("ip", target->ip.c_str())
				.add_text("gateway", target->gateway.c_str())
				.add_bool("success", success)
				.add_text("message", success
					? "route deleted" : error.c_str()));
		}
	}

	response.setContentType("application/json; charset=utf-8");
	if (matched == 0) {
		logger("route delete found no match, key=%s, ip=%s",
			has_key ? key : "*", has_destination ? destination : "*");
		response.setStatus(404);
		root.add_bool("success", false)
			.add_text("message", "no matching routes found")
			.add_number("count", 0)
			.add_number("succeeded", 0)
			.add_child("routes", results);
		return response.write(json);
	}

	bool all_succeeded = matched == succeeded;
	logger("route delete completed, key=%s, ip=%s, matched=%lu, "
		"succeeded=%lu", has_key ? key : "*",
		has_destination ? destination : "*",
		static_cast<unsigned long>(matched),
		static_cast<unsigned long>(succeeded));
	response.setStatus(all_succeeded ? 200 : (succeeded == 0 ? 500 : 207));
	root.add_bool("success", all_succeeded)
		.add_text("message", all_succeeded
			? "all matching routes deleted" : "one or more routes failed")
		.add_number("count", static_cast<long long>(matched))
		.add_number("succeeded", static_cast<long long>(succeeded))
		.add_child("routes", results);
	return response.write(json);
}

bool health(HttpRequest&, HttpResponse& response)
{
	return reply_json(response, 200, true, "ok");
}

bool load_html_template(std::string& content)
{
	std::ifstream input("html/index.html", std::ios::in | std::ios::binary);
	if (!input.is_open()) {
		return false;
	}
	std::istreambuf_iterator<char> begin(input);
	std::istreambuf_iterator<char> end;
	content.assign(begin, end);
	return input.good() || input.eof();
}

bool route_page(HttpRequest&, HttpResponse& response)
{
	std::string page;
	if (!load_html_template(page)) {
		logger_error("load HTML template html/index.html failed");
		return reply_json(response, 500, false,
			"failed to load html/index.html");
	}
	acl::string html(page.c_str());

	response.setStatus(200);
	response.setContentType("text/html; charset=utf-8");
	response.setContentLength(html.size());
	return response.write(html);
}

bool route_list(HttpRequest&, HttpResponse& response)
{
	std::vector<route_entry> routes;
	route_manager::list(routes);

	response.setStatus(200);
	response.setContentType("application/json; charset=utf-8");
	acl::json json;
	acl::json_node& root = json.get_root();
	root.add_bool("success", true)
		.add_number("count", static_cast<long long>(routes.size()));
	acl::json_node& groups = json.create_node(true);
	for (std::vector<route_entry>::const_iterator it = routes.begin();
		it != routes.end(); ++it) {
		acl::json_node& targets = json.create_node(true);
		for (std::vector<route_target>::const_iterator target
			= it->targets.begin(); target != it->targets.end(); ++target) {
			targets.add_child(json.create_node()
				.add_text("ip", target->ip.c_str())
				.add_text("gateway", target->gateway.c_str())
				.add_number("ttl", target->ttl)
				.add_number("expires_at", target->expires_at));
		}
		groups.add_child(json.create_node()
			.add_text("key", it->key.c_str())
			.add_number("count", static_cast<long long>(it->targets.size()))
			.add_child("ips", targets));
	}
	root.add_child("routes", groups);
	return response.write(json);
}

bool system_route_list(HttpRequest&, HttpResponse& response)
{
	std::vector<system_route_entry> routes;
	acl::string error;
	if (!route_manager::list_system(routes, error)) {
		logger_error("list system routes failed, error=%s", error.c_str());
		return reply_json(response, 500, false, error.c_str());
	}

	response.setStatus(200);
	response.setContentType("application/json; charset=utf-8");
	acl::json json;
	acl::json_node& root = json.get_root();
	acl::json_node& items = json.create_node(true);
	for (std::vector<system_route_entry>::const_iterator it = routes.begin();
		it != routes.end(); ++it) {
		items.add_child(json.create_node()
			.add_text("ip", it->ip.c_str())
			.add_text("gateway", it->gateway.c_str())
			.add_text("interface", it->interface_name.c_str()));
	}
	root.add_bool("success", true)
		.add_number("count", static_cast<long long>(routes.size()))
		.add_child("routes", items);
	return response.write(json);
}

bool system_route_delete(HttpRequest& request, HttpResponse& response)
{
	const char* destination = parameter(request, "ip", "target");
	const char* gateway = parameter(request, "gateway", "route");
	if (!route_manager::valid_ipv4(destination)) {
		return reply_json(response, 400, false,
			"parameter 'ip' must be a valid IPv4 address");
	}
	if (!route_manager::valid_ipv4(gateway)) {
		return reply_json(response, 400, false,
			"parameter 'gateway' must be a valid IPv4 address");
	}

	acl::string error;
	if (!route_manager::remove_system(destination, gateway, error)) {
		logger_error("system route delete failed, ip=%s, gateway=%s, error=%s",
			destination, gateway, error.c_str());
		return reply_json(response, 500, false, error.c_str(), destination,
			gateway);
	}

	logger("system route delete succeeded, ip=%s, gateway=%s",
		destination, gateway);
	return reply_json(response, 200, true, "system route deleted",
		destination, gateway);
}

} // namespace

void register_route_service(http_service& service)
{
	service.Get("/", route_page)
		.Get("/health", health)
		.Get("/routes", route_list)
		.Get("/system-routes", system_route_list)
		.Post("/route", route_add)
		.Delete("/route", route_delete)
		.Delete("/system-route", system_route_delete);
}
