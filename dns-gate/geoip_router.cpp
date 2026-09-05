#include "stdafx.h"
#include "geoip_router.h"

#include <maxminddb.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace {

MMDB_s country_database;
bool database_open = false;
std::set<std::string> routed_countries;
std::string ip_router_addr;
std::string route_gateway;
int request_timeout = 3;
int route_ttl = 600;

std::string uppercase(const char* value, size_t length)
{
	std::string result(value, length);
	std::transform(result.begin(), result.end(), result.begin(),
		[](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
	return result;
}

void parse_countries(const char* value)
{
	routed_countries.clear();
	acl::string input(value == NULL ? "" : value);
	const std::vector<acl::string>& values = input.split2(",; \t\r\n");
	for (std::vector<acl::string>::const_iterator it = values.begin();
		it != values.end(); ++it) {
		std::string country = uppercase(it->c_str(), it->size());
		if (country.size() == 2) {
			routed_countries.insert(country);
		} else {
			logger_warn("ignore invalid GeoIP country code=%s", it->c_str());
		}
	}
}

bool lookup_country(const char* ip, std::string& country)
{
	int gai_error = 0;
	int mmdb_error = MMDB_SUCCESS;
	MMDB_lookup_result_s result = MMDB_lookup_string(&country_database, ip,
		&gai_error, &mmdb_error);
	if (gai_error != 0) {
		logger_error("GeoIP lookup address error, ip=%s, error=%s", ip,
			gai_strerror(gai_error));
		return false;
	}
	if (mmdb_error != MMDB_SUCCESS) {
		logger_error("GeoIP lookup database error, ip=%s, error=%s", ip,
			MMDB_strerror(mmdb_error));
		return false;
	}
	if (!result.found_entry) {
		return false;
	}

	MMDB_entry_data_s data;
	int status = MMDB_get_value(&result.entry, &data, "country", "iso_code",
		NULL);
	if (status != MMDB_SUCCESS) {
		logger_error("GeoIP read country error, ip=%s, error=%s", ip,
			MMDB_strerror(status));
		return false;
	}
	if (!data.has_data || data.type != MMDB_DATA_TYPE_UTF8_STRING) {
		return false;
	}

	country = uppercase(data.utf8_string, data.data_size);
	return true;
}

void append_encoded_parameter(acl::string& url, const char* name,
	const char* value, bool first)
{
	acl::string encoded;
	encoded.url_encode(value);
	url.format_append("%c%s=%s", first ? '?' : '&', name, encoded.c_str());
}

bool add_routes(const char* name, const std::vector<std::string>& addresses)
{
	acl::string values;
	for (std::vector<std::string>::const_iterator it = addresses.begin();
		it != addresses.end(); ++it) {
		if (!values.empty()) {
			values.push_back(',');
		}
		values.append(it->c_str());
	}

	acl::string key(name);
	key.lower();
	acl::string url("/route");
	append_encoded_parameter(url, "key", key.c_str(), true);
	append_encoded_parameter(url, "ips", values.c_str(), false);
	append_encoded_parameter(url, "gateway", route_gateway.c_str(), false);
	acl::string ttl;
	ttl.format("%d", route_ttl);
	append_encoded_parameter(url, "ttl", ttl.c_str(), false);

	acl::http_request request(ip_router_addr.c_str(), request_timeout,
		request_timeout);
	request.request_header()
		.set_method(acl::HTTP_METHOD_POST)
		.set_url(url.c_str(), false)
		.set_host(ip_router_addr.c_str())
		.set_keep_alive(false);
	if (!request.request("", 0)) {
		logger_error("send route request failed, router=%s, key=%s, ips=%s, "
			"error=%s", ip_router_addr.c_str(), key.c_str(), values.c_str(),
			acl::last_serror());
		return false;
	}

	int status = request.http_status();
	acl::string body;
	if (!request.get_body(body)) {
		logger_error("read route response failed, router=%s, key=%s, status=%d",
			ip_router_addr.c_str(), key.c_str(), status);
		return false;
	}
	if (status != 200) {
		logger_error("route request rejected, router=%s, key=%s, ips=%s, "
			"status=%d, response=%s", ip_router_addr.c_str(), key.c_str(),
			values.c_str(), status, body.c_str());
		return false;
	}

	logger("route request succeeded, key=%s, ips=%s, gateway=%s, ttl=%d",
		key.c_str(), values.c_str(), route_gateway.c_str(), route_ttl);
	return true;
}

} // namespace

bool geoip_router_start(const char* database_path, const char* countries,
	const char* router_addr, const char* gateway, int timeout, int ttl)
{
	geoip_router_stop();
	parse_countries(countries);
	ip_router_addr = router_addr == NULL ? "" : router_addr;
	route_gateway = gateway == NULL ? "" : gateway;
	request_timeout = timeout;
	route_ttl = ttl;

	if (database_path == NULL || *database_path == 0) {
		logger_warn("GeoIP routing disabled: geoip_database is empty");
		return false;
	}
	if (routed_countries.empty()) {
		logger_warn("GeoIP routing disabled: geoip_countries is empty");
		return false;
	}
	if (ip_router_addr.empty() || route_gateway.empty()) {
		logger_warn("GeoIP routing disabled: ip_router_addr or "
			"ip_router_gateway is empty");
		return false;
	}

	int status = MMDB_open(database_path, MMDB_MODE_MMAP, &country_database);
	if (status != MMDB_SUCCESS) {
		logger_error("open GeoIP database failed, file=%s, error=%s",
			database_path, MMDB_strerror(status));
		return false;
	}
	database_open = true;
	logger("GeoIP routing enabled, database=%s, countries=%s, router=%s, "
		"gateway=%s, ttl=%d", database_path, countries, router_addr, gateway,
		route_ttl);
	return true;
}

void geoip_router_stop(void)
{
	if (database_open) {
		MMDB_close(&country_database);
		database_open = false;
	}
}

void geoip_route_dns_result(const char* name,
	const acl::rfc1035_response& response)
{
	if (!database_open || name == NULL || *name == 0) {
		return;
	}

	std::set<std::string> unique;
	std::vector<std::string> matched;
	const std::vector<acl::string>& addresses = response.get_addrs4a();
	for (std::vector<acl::string>::const_iterator it = addresses.begin();
		it != addresses.end(); ++it) {
		std::string country;
		if (!lookup_country(it->c_str(), country)) {
			continue;
		}
		logger("GeoIP result, name=%s, ip=%s, country=%s", name,
			it->c_str(), country.c_str());
		if (routed_countries.find(country) != routed_countries.end()
			&& unique.insert(it->c_str()).second) {
			matched.push_back(it->c_str());
		}
	}

	if (!matched.empty()) {
		add_routes(name, matched);
	}
}
