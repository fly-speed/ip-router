#pragma once

void dgate_service_start(void);

void dgate_push_request(acl::socket_stream* server, const char* peer_addr,
	const char* data, size_t dlen);

// 直接向配置的上游 DNS 查询 A 记录，供 HTTP 诊断接口复用。
bool dgate_resolve_domain(const char* name, acl::rfc1035_response& response,
	acl::string& error);
