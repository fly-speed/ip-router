#pragma once

#include <string>

namespace acl {
class rfc1035_response;
}

bool geoip_router_start(const char* database_path, const char* countries,
	const char* router_addr, const char* gateway, int timeout, int ttl);

void geoip_router_stop(void);

void geoip_route_dns_result(const char* name,
	const acl::rfc1035_response& response);

bool geoip_database_ready(void);
bool geoip_routing_ready(void);
bool geoip_lookup_address(const char* ip, std::string& country,
	bool& route_matched);
