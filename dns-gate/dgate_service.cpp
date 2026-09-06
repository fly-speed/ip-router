#include "stdafx.h"
#include <atomic>
#include <thread>
#include "dgate_service.h"
#include "geoip_router.h"
#include "master_service.h"

namespace {

class request_message {
public:
	request_message(acl::socket_stream* server, const char* peer_addr,
		const char* data, size_t dlen)
	: server_(server)
	, peer_addr_(peer_addr)
	, data_(data, dlen)
	{
	}

	acl::socket_stream* server_;
	acl::string peer_addr_;
	acl::string data_;
};

acl::fiber_tbox<request_message>* request_box;
std::atomic<unsigned int> query_sequence(1);

void handle_request(request_message& message)
{
	acl::rfc1035_request request;
	if (!request.parse_request(message.data_.c_str(), message.data_.size())) {
		logger_warn("invalid DNS request from %s, size=%lu",
			message.peer_addr_.c_str(),
			static_cast<unsigned long>(message.data_.size()));
		return;
	}

	const char* name = request.get_name();
	acl::socket_stream upstream;
	if (!upstream.bind_udp("0.0.0.0|0")) {
		logger_error("bind UDP socket error=%s, name=%s",
			acl::last_serror(), name);
		return;
	}
	upstream.set_rw_timeout(var_cfg_upstream_timeout);

	if (upstream.sendto(message.data_.c_str(), message.data_.size(),
		var_cfg_upstream_addr, 0) == -1) {
		logger_error("send DNS request to %s error=%s, name=%s",
			var_cfg_upstream_addr, acl::last_serror(), name);
		return;
	}

	char reply[65536];
	int length = upstream.read(reply, sizeof(reply), false);
	if (length == -1) {
		logger_error("read DNS response from %s error=%s, name=%s",
			var_cfg_upstream_addr, acl::last_serror(), name);
		return;
	}

	acl::rfc1035_response response;
	if (!response.parse_reply(reply, static_cast<size_t>(length))) {
		logger_error("invalid DNS response from %s, name=%s",
			var_cfg_upstream_addr, name);
		return;
	}
	if (response.get_qid() != request.get_qid()) {
		logger_error("DNS response id mismatch from %s, name=%s",
			var_cfg_upstream_addr, name);
		return;
	}

	// 必须先完成 GeoIP 判断及路由添加请求，再向 DNS 客户端返回解析结果。
	geoip_route_dns_result(name, response);

	acl::socket_stream client;
	client.open(message.server_->sock_handle(), true);
	if (client.sendto(reply, static_cast<size_t>(length),
		message.peer_addr_, 0) == -1) {
		logger_error("send DNS response to %s error=%s, name=%s",
			message.peer_addr_.c_str(), acl::last_serror(), name);
	}
	client.unbind_sock();
}

void consume_requests(acl::fiber_tbox<request_message>* box)
{
	for (;;) {
		request_message* message = box->pop();
		if (message == NULL) {
			logger_error("pop request message error=%s", acl::last_serror());
			break;
		}

		go[=] {
			handle_request(*message);
			delete message;
		};
	}
}

void service_main(acl::fiber_tbox<request_message>* box)
{
	go[=] {
		consume_requests(box);
	};
	acl::fiber::schedule();
}

} // namespace

bool dgate_resolve_domain(const char* name, acl::rfc1035_response& response,
	acl::string& error)
{
	char query[512];
	unsigned short qid = static_cast<unsigned short>(query_sequence.fetch_add(1));
	acl::rfc1035_request request;
	request.set_name(name).set_qid(qid).set_type(acl::rfc1035_type_a);
	size_t query_length = request.build_query(query, sizeof(query));
	if (query_length == 0) {
		error = "failed to build DNS query";
		return false;
	}

	acl::socket_stream upstream;
	if (!upstream.bind_udp("0.0.0.0|0")) {
		error.format("bind UDP socket failed: %s", acl::last_serror());
		return false;
	}
	upstream.set_rw_timeout(var_cfg_upstream_timeout);
	if (upstream.sendto(query, query_length, var_cfg_upstream_addr, 0) == -1) {
		error.format("send DNS query to %s failed: %s", var_cfg_upstream_addr,
			acl::last_serror());
		return false;
	}

	char reply[65536];
	int length = upstream.read(reply, sizeof(reply), false);
	if (length == -1) {
		error.format("read DNS response from %s failed: %s",
			var_cfg_upstream_addr, acl::last_serror());
		return false;
	}
	if (!response.parse_reply(reply, static_cast<size_t>(length))) {
		error = "invalid DNS response";
		return false;
	}
	if (response.get_qid() != qid) {
		error = "DNS response id mismatch";
		return false;
	}
	return true;
}

void dgate_service_start(void)
{
	request_box = new acl::fiber_tbox<request_message>;
	std::thread worker(service_main, request_box);
	worker.detach();
}

void dgate_push_request(acl::socket_stream* server, const char* peer_addr,
	const char* data, size_t dlen)
{
	request_box->push(new request_message(server, peer_addr, data, dlen));
}
