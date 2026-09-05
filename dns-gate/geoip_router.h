#pragma once

namespace acl {
class rfc1035_response;
}

bool geoip_router_start(const char* database_path, const char* countries,
	const char* router_addr, const char* gateway, int timeout, int ttl);

void geoip_router_stop(void);

void geoip_route_dns_result(const char* name,
	const acl::rfc1035_response& response);
