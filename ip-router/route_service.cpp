#include "stdafx.h"
#include "http_service.h"
#include "route_manager.h"
#include "route_service.h"

#include <errno.h>
#include <set>
#include <stdlib.h>

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
		logger_error("route %s failed, ip=%s, gateway=%s, error=%s",
			add ? "add" : "delete", destination, gateway, error.c_str());
		return reply_json(response, 500, false, error.c_str(), destination,
			gateway, key);
	}

	logger("route %s succeeded, ip=%s, gateway=%s, key=%s",
		add ? "add" : "delete", destination, gateway,
		key ? key : "");
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
			logger("route add succeeded, ip=%s, gateway=%s, key=%s",
				it->c_str(), gateway, key ? key : "");
		} else {
			logger_error("route add failed, ip=%s, gateway=%s, error=%s",
				it->c_str(), gateway, error.c_str());
		}
	}

	bool all_succeeded = succeeded == destinations.size();
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
			bool success = route_manager::remove(group->key.c_str(),
				target->ip.c_str(), target->gateway.c_str(), error);
			if (success) {
				++succeeded;
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
		response.setStatus(404);
		root.add_bool("success", false)
			.add_text("message", "no matching routes found")
			.add_number("count", 0)
			.add_number("succeeded", 0)
			.add_child("routes", results);
		return response.write(json);
	}

	bool all_succeeded = matched == succeeded;
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

} // namespace

void register_route_service(http_service& service)
{
	route_manager::start();
	service.Get("/health", health)
		.Get("/routes", route_list)
		.Post("/route", route_add)
		.Delete("/route", route_delete);
}
