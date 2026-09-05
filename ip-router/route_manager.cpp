#include "stdafx.h"
#include "route_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <errno.h>
#include <limits>
#include <map>
#include <mutex>
#include <string.h>
#include <thread>
#include <time.h>

#if defined(__linux__)
# include <arpa/inet.h>
# include <linux/netlink.h>
# include <linux/rtnetlink.h>
# include <sys/socket.h>
# include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
# include <arpa/inet.h>
# include <net/route.h>
# include <netinet/in.h>
# include <sys/socket.h>
# include <sys/time.h>
# include <unistd.h>
#endif

namespace {

std::atomic<unsigned int> route_sequence(0);
std::mutex routes_mutex;
std::condition_variable routes_changed;
std::thread expiration_worker;
bool worker_started = false;
bool worker_stopping = false;

class route_metadata {
public:
	route_metadata(void) : ttl(0), expires_at(0), last_attempt(0) {}
	route_metadata(const char* next_hop, long long configured_ttl,
		time_t expiration_time)
	: gateway(next_hop), ttl(configured_ttl), expires_at(expiration_time),
	  last_attempt(0) {}

	acl::string gateway;
	long long ttl;
	time_t expires_at;
	time_t last_attempt;
};

typedef std::map<acl::string, route_metadata> key_routes;
std::map<acl::string, key_routes> installed_routes;

void set_error(acl::string& error, const char* operation, int code)
{
	error.format("%s failed: %s (errno=%d)", operation, strerror(code), code);
}

#if defined(__linux__)

bool add_attribute(struct nlmsghdr* header, size_t capacity, int type,
	const void* data, size_t length)
{
	size_t attribute_length = RTA_LENGTH(length);
	size_t message_length = NLMSG_ALIGN(header->nlmsg_len);
	if (message_length + RTA_ALIGN(attribute_length) > capacity) {
		return false;
	}

	struct rtattr* attribute = reinterpret_cast<struct rtattr*>(
		reinterpret_cast<char*>(header) + message_length);
	attribute->rta_type = type;
	attribute->rta_len = static_cast<unsigned short>(attribute_length);
	memcpy(RTA_DATA(attribute), data, length);
	header->nlmsg_len = static_cast<unsigned int>(
		message_length + RTA_ALIGN(attribute_length));
	return true;
}

bool change_route(int command, const char* destination, const char* gateway,
	acl::string& error)
{
	unsigned int sequence = ++route_sequence;
	struct in_addr destination_addr;
	struct in_addr gateway_addr;
	if (inet_pton(AF_INET, destination, &destination_addr) != 1
		|| inet_pton(AF_INET, gateway, &gateway_addr) != 1) {
		error = "invalid IPv4 address";
		return false;
	}

	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		set_error(error, "open NETLINK_ROUTE socket", errno);
		return false;
	}

	struct sockaddr_nl local;
	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	if (bind(fd, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) == -1) {
		int code = errno;
		close(fd);
		set_error(error, "bind NETLINK_ROUTE socket", code);
		return false;
	}

	struct {
		struct nlmsghdr header;
		struct rtmsg route;
		char attributes[128];
	} request;
	memset(&request, 0, sizeof(request));
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	request.header.nlmsg_type = static_cast<unsigned short>(command);
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	if (command == RTM_NEWROUTE) {
		request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
	}
	request.header.nlmsg_seq = sequence;
	request.route.rtm_family = AF_INET;
	request.route.rtm_dst_len = 32;
	request.route.rtm_table = RT_TABLE_MAIN;
	request.route.rtm_protocol = RTPROT_STATIC;
	request.route.rtm_scope = RT_SCOPE_UNIVERSE;
	request.route.rtm_type = RTN_UNICAST;

	if (!add_attribute(&request.header, sizeof(request), RTA_DST,
		&destination_addr, sizeof(destination_addr))
		|| !add_attribute(&request.header, sizeof(request), RTA_GATEWAY,
			&gateway_addr, sizeof(gateway_addr))) {
		close(fd);
		error = "route message is too large";
		return false;
	}

	struct sockaddr_nl kernel;
	memset(&kernel, 0, sizeof(kernel));
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, &request, request.header.nlmsg_len, 0,
		reinterpret_cast<struct sockaddr*>(&kernel), sizeof(kernel)) == -1) {
		int code = errno;
		close(fd);
		set_error(error, "send route request", code);
		return false;
	}

	char response[4096];
	ssize_t length = recv(fd, response, sizeof(response), 0);
	if (length == -1) {
		int code = errno;
		close(fd);
		set_error(error, "receive route response", code);
		return false;
	}
	close(fd);

	for (struct nlmsghdr* header = reinterpret_cast<struct nlmsghdr*>(response);
		NLMSG_OK(header, static_cast<unsigned int>(length));
		header = NLMSG_NEXT(header, length)) {
		if (header->nlmsg_seq != sequence || header->nlmsg_type != NLMSG_ERROR) {
			continue;
		}
		const struct nlmsgerr* result = reinterpret_cast<const struct nlmsgerr*>(
			NLMSG_DATA(header));
		if (result->error == 0) {
			return true;
		}
		set_error(error, "change route", -result->error);
		return false;
	}

	error = "NETLINK_ROUTE returned no acknowledgement";
	return false;
}

#elif defined(__APPLE__) || defined(__FreeBSD__)

bool change_route(int command, const char* destination, const char* gateway,
	acl::string& error)
{
	int sequence = static_cast<int>(++route_sequence);
	struct in_addr destination_addr;
	struct in_addr gateway_addr;
	if (inet_pton(AF_INET, destination, &destination_addr) != 1
		|| inet_pton(AF_INET, gateway, &gateway_addr) != 1) {
		error = "invalid IPv4 address";
		return false;
	}

	int fd = socket(PF_ROUTE, SOCK_RAW, AF_INET);
	if (fd == -1) {
		set_error(error, "open PF_ROUTE socket", errno);
		return false;
	}

	struct timeval timeout;
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	struct route_message {
		struct rt_msghdr header;
		struct sockaddr_in destination;
		struct sockaddr_in gateway;
	} message;
	memset(&message, 0, sizeof(message));

	message.header.rtm_msglen = sizeof(message);
	message.header.rtm_version = RTM_VERSION;
	message.header.rtm_type = command;
	message.header.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_HOST | RTF_STATIC;
	message.header.rtm_addrs = RTA_DST | RTA_GATEWAY;
	message.header.rtm_pid = getpid();
	message.header.rtm_seq = sequence;

	message.destination.sin_len = sizeof(message.destination);
	message.destination.sin_family = AF_INET;
	message.destination.sin_addr = destination_addr;
	message.gateway.sin_len = sizeof(message.gateway);
	message.gateway.sin_family = AF_INET;
	message.gateway.sin_addr = gateway_addr;

	if (write(fd, &message, sizeof(message)) == -1) {
		int code = errno;
		close(fd);
		set_error(error, "write PF_ROUTE request", code);
		return false;
	}

	for (;;) {
		char response[2048];
		ssize_t length = read(fd, response, sizeof(response));
		if (length == -1) {
			int code = errno;
			close(fd);
			set_error(error, "read PF_ROUTE response", code);
			return false;
		}
		if (static_cast<size_t>(length) < sizeof(struct rt_msghdr)) {
			continue;
		}

		const struct rt_msghdr* header =
			reinterpret_cast<const struct rt_msghdr*>(response);
		if (header->rtm_pid != getpid() || header->rtm_seq != sequence) {
			continue;
		}
		close(fd);
		if (header->rtm_errno == 0) {
			return true;
		}
		set_error(error, "change route", header->rtm_errno);
		return false;
	}
}

#else

bool change_route(int, const char*, const char*, acl::string& error)
{
	error = "route management is unsupported on this operating system";
	return false;
}

#endif

bool delete_system_route(const char* destination, const char* gateway,
	acl::string& error)
{
#if defined(__linux__)
	return change_route(RTM_DELROUTE, destination, gateway, error);
#elif defined(__APPLE__) || defined(__FreeBSD__)
	return change_route(RTM_DELETE, destination, gateway, error);
#else
	return change_route(0, destination, gateway, error);
#endif
}

void expire_routes(void)
{
	std::unique_lock<std::mutex> lock(routes_mutex);
	while (!worker_stopping) {
		time_t now = time(NULL);
		for (std::map<acl::string, key_routes>::iterator group
			= installed_routes.begin(); group != installed_routes.end();) {
			for (key_routes::iterator target = group->second.begin();
				target != group->second.end();) {
				route_metadata& metadata = target->second;
				if (metadata.expires_at <= 0 || metadata.expires_at > now
					|| (metadata.last_attempt > 0
						&& now - metadata.last_attempt < 30)) {
					++target;
					continue;
				}

				acl::string error;
				metadata.last_attempt = now;
				bool referenced_elsewhere = false;
				for (std::map<acl::string, key_routes>::const_iterator other
					= installed_routes.begin(); other != installed_routes.end();
					++other) {
					if (other == group) {
						continue;
					}
					if (other->second.find(target->first)
						!= other->second.end()) {
						referenced_elsewhere = true;
						break;
					}
				}
				bool deleted = referenced_elsewhere || delete_system_route(
					target->first.c_str(), metadata.gateway.c_str(), error);
				if (deleted) {
					logger("expired route %s, key=%s, ip=%s, gateway=%s",
						referenced_elsewhere ? "reference removed" : "deleted",
						group->first.c_str(), target->first.c_str(),
						metadata.gateway.c_str());
					target = group->second.erase(target);
				} else {
					logger_error("delete expired route failed, key=%s, ip=%s, "
						"gateway=%s, error=%s", group->first.c_str(),
						target->first.c_str(), metadata.gateway.c_str(),
						error.c_str());
					++target;
				}
			}
			if (group->second.empty()) {
				group = installed_routes.erase(group);
			} else {
				++group;
			}
		}

		routes_changed.wait_for(lock, std::chrono::seconds(1), [] {
			return worker_stopping;
		});
	}
}

} // namespace

void route_manager::start(void)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	if (worker_started) {
		return;
	}
	worker_started = true;
	worker_stopping = false;
	expiration_worker = std::thread(expire_routes);
	std::atexit(route_manager::stop);
}

void route_manager::stop(void)
{
	{
		std::lock_guard<std::mutex> guard(routes_mutex);
		if (!worker_started) {
			return;
		}
		worker_stopping = true;
		routes_changed.notify_all();
	}
	if (expiration_worker.joinable()) {
		expiration_worker.join();
	}
	std::lock_guard<std::mutex> guard(routes_mutex);
	worker_started = false;
}

bool route_manager::valid_ipv4(const char* value)
{
	if (value == NULL || *value == 0) {
		return false;
	}
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	return acl_inet_pton(AF_INET, value,
		reinterpret_cast<struct sockaddr*>(&address)) > 0;
}

bool route_manager::add(const char* destination, const char* gateway,
	const char* key, long long ttl, acl::string& error)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	time_t now = time(NULL);
	time_t expires_at = 0;
	if (ttl > 0) {
		long long maximum = static_cast<long long>(
			std::numeric_limits<time_t>::max());
		if (ttl > maximum - static_cast<long long>(now)) {
			error = "parameter 'ttl' is too large";
			return false;
		}
		expires_at = now + static_cast<time_t>(ttl);
	}
	std::map<acl::string, key_routes>::iterator existing_group
		= installed_routes.find(key);
	if (existing_group != installed_routes.end()) {
		key_routes::iterator existing
			= existing_group->second.find(destination);
		if (existing != existing_group->second.end()
			&& existing->second.gateway == gateway) {
			existing->second = route_metadata(gateway,
				ttl > 0 ? ttl : 0, expires_at);
			routes_changed.notify_all();
			return true;
		}
	}
	for (std::map<acl::string, key_routes>::const_iterator group
		= installed_routes.begin(); group != installed_routes.end(); ++group) {
		key_routes::const_iterator existing = group->second.find(destination);
		if (existing == group->second.end()) {
			continue;
		}
		if (existing->second.gateway != gateway) {
			error.format("IP %s already uses gateway %s under key %s",
				destination, existing->second.gateway.c_str(),
				group->first.c_str());
			return false;
		}
		installed_routes[key][destination]
			= route_metadata(gateway, ttl > 0 ? ttl : 0, expires_at);
		routes_changed.notify_all();
		return true;
	}
	bool success;
#if defined(__linux__)
	success = change_route(RTM_NEWROUTE, destination, gateway, error);
#elif defined(__APPLE__) || defined(__FreeBSD__)
	success = change_route(RTM_ADD, destination, gateway, error);
#else
	success = change_route(0, destination, gateway, error);
#endif
	if (success) {
		installed_routes[key][destination]
			= route_metadata(gateway, ttl > 0 ? ttl : 0, expires_at);
		routes_changed.notify_all();
	}
	return success;
}

bool route_manager::remove(const char* key, const char* destination,
	const char* gateway, acl::string& error)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	std::map<acl::string, key_routes>::iterator group
		= installed_routes.find(key);
	if (group == installed_routes.end()) {
		error.format("route key not found: %s", key);
		return false;
	}
	key_routes::iterator target = group->second.find(destination);
	if (target == group->second.end() || target->second.gateway != gateway) {
		error.format("route not found under key %s: %s via %s",
			key, destination, gateway);
		return false;
	}

	bool referenced_elsewhere = false;
	for (std::map<acl::string, key_routes>::const_iterator it
		= installed_routes.begin(); it != installed_routes.end(); ++it) {
		if (it == group) {
			continue;
		}
		if (it->second.find(destination) != it->second.end()) {
			referenced_elsewhere = true;
			break;
		}
	}

	bool success = referenced_elsewhere
		? true : delete_system_route(destination, gateway, error);
	if (success) {
		group->second.erase(target);
		if (group->second.empty()) {
			installed_routes.erase(group);
		}
		routes_changed.notify_all();
	}
	return success;
}

void route_manager::list(std::vector<route_entry>& routes)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	routes.clear();
	routes.reserve(installed_routes.size());
	for (std::map<acl::string, key_routes>::const_iterator group
		= installed_routes.begin(); group != installed_routes.end(); ++group) {
		routes.push_back(route_entry(group->first.c_str()));
		route_entry& entry = routes.back();
		entry.targets.reserve(group->second.size());
		for (key_routes::const_iterator target = group->second.begin();
			target != group->second.end(); ++target) {
			entry.targets.push_back(route_target(target->first.c_str(),
				target->second.gateway.c_str(), target->second.ttl,
				static_cast<long long>(target->second.expires_at)));
		}
	}
}
