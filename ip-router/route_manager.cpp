#include "stdafx.h"
#include "route_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <errno.h>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <thread>
#include <time.h>

#if defined(__linux__)
# include <arpa/inet.h>
# include <linux/netlink.h>
# include <linux/rtnetlink.h>
# include <net/if.h>
# include <sys/socket.h>
# include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
# include <arpa/inet.h>
# include <net/route.h>
# include <net/if.h>
# include <netinet/in.h>
# include <sys/socket.h>
# include <sys/sysctl.h>
# include <unistd.h>
#endif

namespace {

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
bool address_text(const struct in_addr& address, acl::string& output)
{
	char buffer[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) == NULL) {
		return false;
	}
	output = buffer;
	return true;
}
#endif

std::atomic<unsigned int> route_sequence(0);
std::mutex routes_mutex;
std::condition_variable routes_changed;
std::thread expiration_worker;
bool worker_started = false;
bool worker_stopping = false;
std::string routes_file("routes.db");
std::string global_route_file("routes.db.global");
acl::string global_gateway;
bool force_global_gateway = false;

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

void set_error(acl::string& error, const char* operation, int code);

bool save_global_route_locked(acl::string& error)
{
	std::string temporary(global_route_file);
	temporary.append(".tmp");
	std::ofstream output(temporary.c_str(),
		std::ios::out | std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		set_error(error, "open temporary global route settings", errno);
		return false;
	}

	output << "# ip-router global route v1\n";
	if (!global_gateway.empty()) {
		output << global_gateway.c_str() << '\t'
			<< (force_global_gateway ? 1 : 0) << '\n';
	}
	output.flush();
	if (!output.good()) {
		output.close();
		error = "write temporary global route settings failed";
		return false;
	}
	output.close();

#if defined(_WIN32) || defined(_WIN64)
	::remove(global_route_file.c_str());
#endif
	if (::rename(temporary.c_str(), global_route_file.c_str()) != 0) {
		set_error(error, "replace global route settings", errno);
		return false;
	}
	return true;
}

bool load_global_route_locked(acl::string& error)
{
	std::ifstream input(global_route_file.c_str(),
		std::ios::in | std::ios::binary);
	if (!input.is_open()) {
		if (errno == ENOENT) {
			return true;
		}
		set_error(error, "open global route settings", errno);
		return false;
	}

	std::string header;
	if (!std::getline(input, header)
		|| header != "# ip-router global route v1") {
		error = "invalid global route settings header";
		return false;
	}
	std::string line;
	if (!std::getline(input, line) || line.empty()) {
		global_gateway = "";
		force_global_gateway = false;
		return true;
	}
	std::string::size_type separator = line.find('\t');
	if (separator == std::string::npos
		|| line.find('\t', separator + 1) != std::string::npos) {
		error = "invalid global route settings record";
		return false;
	}
	std::string gateway = line.substr(0, separator);
	std::string force = line.substr(separator + 1);
	if (!route_manager::valid_ipv4(gateway.c_str())
		|| (force != "0" && force != "1")) {
		error = "invalid global route settings values";
		return false;
	}
	global_gateway = gateway.c_str();
	force_global_gateway = force == "1";
	return true;
}

bool save_routes_locked(acl::string& error)
{
	std::string temporary(routes_file);
	temporary.append(".tmp");
	std::ofstream output(temporary.c_str(),
		std::ios::out | std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		set_error(error, "open temporary route database", errno);
		return false;
	}

	output << "# ip-router routes v1\n";
	for (std::map<acl::string, key_routes>::const_iterator group
		= installed_routes.begin(); group != installed_routes.end(); ++group) {
		for (key_routes::const_iterator target = group->second.begin();
			target != group->second.end(); ++target) {
			output << group->first.c_str() << '\t' << target->first.c_str()
				<< '\t' << target->second.gateway.c_str() << '\t'
				<< target->second.ttl << '\t' << target->second.expires_at
				<< '\n';
		}
	}
	output.flush();
	if (!output.good()) {
		output.close();
		error = "write temporary route database failed";
		return false;
	}
	output.close();

#if defined(_WIN32) || defined(_WIN64)
	// Windows 的 rename 不能覆盖现有文件；该平台当前不支持系统路由操作，
	// 这里仍保留可编译的持久化实现。
	::remove(routes_file.c_str());
#endif
	if (::rename(temporary.c_str(), routes_file.c_str()) != 0) {
		set_error(error, "replace route database", errno);
		return false;
	}
	return true;
}

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

bool list_system_routes(std::vector<system_route_entry>& routes,
	acl::string& error)
{
	unsigned int sequence = ++route_sequence;
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1) {
		set_error(error, "open NETLINK_ROUTE socket", errno);
		return false;
	}

	struct sockaddr_nl local;
	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	if (bind(fd, reinterpret_cast<struct sockaddr*>(&local),
		sizeof(local)) == -1) {
		int code = errno;
		close(fd);
		set_error(error, "bind NETLINK_ROUTE socket", code);
		return false;
	}

	struct {
		struct nlmsghdr header;
		struct rtmsg route;
	} request;
	memset(&request, 0, sizeof(request));
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	request.header.nlmsg_type = RTM_GETROUTE;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.header.nlmsg_seq = sequence;
	request.route.rtm_family = AF_INET;

	struct sockaddr_nl kernel;
	memset(&kernel, 0, sizeof(kernel));
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, &request, request.header.nlmsg_len, 0,
		reinterpret_cast<struct sockaddr*>(&kernel), sizeof(kernel)) == -1) {
		int code = errno;
		close(fd);
		set_error(error, "request system route dump", code);
		return false;
	}

	bool done = false;
	while (!done) {
		char response[16384];
		ssize_t received = recv(fd, response, sizeof(response), 0);
		if (received == -1) {
			int code = errno;
			close(fd);
			set_error(error, "receive system route dump", code);
			return false;
		}
		int remaining = static_cast<int>(received);
		for (struct nlmsghdr* header
			= reinterpret_cast<struct nlmsghdr*>(response);
			NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
			if (header->nlmsg_seq != sequence) {
				continue;
			}
			if (header->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}
			if (header->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr* result =
					reinterpret_cast<const struct nlmsgerr*>(NLMSG_DATA(header));
				int code = result->error == 0 ? EIO : -result->error;
				close(fd);
				set_error(error, "dump system routes", code);
				return false;
			}
			if (header->nlmsg_type != RTM_NEWROUTE) {
				continue;
			}

			const struct rtmsg* route = reinterpret_cast<const struct rtmsg*>(
				NLMSG_DATA(header));
			if (route->rtm_family != AF_INET || route->rtm_dst_len != 32
				|| route->rtm_protocol != RTPROT_STATIC
				|| route->rtm_type != RTN_UNICAST) {
				continue;
			}

			struct in_addr destination;
			struct in_addr gateway;
			memset(&destination, 0, sizeof(destination));
			memset(&gateway, 0, sizeof(gateway));
			bool has_destination = false;
			bool has_gateway = false;
			unsigned int interface_index = 0;
			unsigned int table = route->rtm_table;
			int attributes_length = RTM_PAYLOAD(header);
			for (struct rtattr* attribute = RTM_RTA(route);
				RTA_OK(attribute, attributes_length);
				attribute = RTA_NEXT(attribute, attributes_length)) {
				switch (attribute->rta_type) {
				case RTA_DST:
					if (static_cast<size_t>(RTA_PAYLOAD(attribute))
						>= sizeof(destination)) {
						memcpy(&destination, RTA_DATA(attribute),
							sizeof(destination));
						has_destination = true;
					}
					break;
				case RTA_GATEWAY:
					if (static_cast<size_t>(RTA_PAYLOAD(attribute))
						>= sizeof(gateway)) {
						memcpy(&gateway, RTA_DATA(attribute), sizeof(gateway));
						has_gateway = true;
					}
					break;
				case RTA_OIF:
					if (static_cast<size_t>(RTA_PAYLOAD(attribute))
						>= sizeof(interface_index)) {
						memcpy(&interface_index, RTA_DATA(attribute),
							sizeof(interface_index));
					}
					break;
				case RTA_TABLE:
					if (static_cast<size_t>(RTA_PAYLOAD(attribute)) >= sizeof(table)) {
						memcpy(&table, RTA_DATA(attribute), sizeof(table));
					}
					break;
				default:
					break;
				}
			}
			if (!has_destination || !has_gateway || table != RT_TABLE_MAIN) {
				continue;
			}

			acl::string destination_text;
			acl::string gateway_text;
			if (!address_text(destination, destination_text)
				|| !address_text(gateway, gateway_text)) {
				continue;
			}
			char device[IF_NAMESIZE] = { 0 };
			if (interface_index > 0) {
				if_indextoname(interface_index, device);
			}
			routes.push_back(system_route_entry(destination_text.c_str(),
				gateway_text.c_str(), device));
		}
	}

	close(fd);
	return true;
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

	// PF_ROUTE 的第三个参数是路由协议号，macOS/FreeBSD 均应使用 0。
	// AF_INET 只用于消息内的目标地址类型，不能作为这里的协议号。
	int fd = socket(PF_ROUTE, SOCK_RAW, 0);
	if (fd == -1) {
		set_error(error, "open PF_ROUTE socket", errno);
		return false;
	}

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

	ssize_t written = write(fd, &message, sizeof(message));
	if (written == -1) {
		int code = errno;
		close(fd);
		// 与 Linux 的 NLM_F_REPLACE 保持一致：目标主机路由已存在时，
		// 改用 RTM_CHANGE 更新网关。这也可以接管旧版本已经写入系统、
		// 却因错误等待回包而未保存到内存索引中的路由。
		if (command == RTM_ADD && code == EEXIST) {
			return change_route(RTM_CHANGE, destination, gateway, error);
		}
		set_error(error, "write PF_ROUTE request", code);
		return false;
	}
	if (static_cast<size_t>(written) != sizeof(message)) {
		error.format("write PF_ROUTE request failed: wrote %ld of %lu bytes",
			static_cast<long>(written),
			static_cast<unsigned long>(sizeof(message)));
		close(fd);
		return false;
	}

	// RTM_ADD 和 RTM_DELETE 的执行错误由 write() 同步返回。内核不会像
	// RTM_GET 那样保证再发送一个需要读取的应答；继续 read() 会在 ACL
	// fiber 的非阻塞套接字上等待至超时，最终错误地返回 EAGAIN。
	close(fd);
	return true;
}

size_t route_address_size(const struct sockaddr* address)
{
# if defined(__APPLE__)
	const size_t alignment = sizeof(uint32_t);
# else
	const size_t alignment = sizeof(long);
# endif
	return address->sa_len == 0 ? alignment
		: (address->sa_len + alignment - 1) & ~(alignment - 1);
}

bool is_ipv4_host_mask(const struct sockaddr* address)
{
	const size_t offset = offsetof(struct sockaddr_in, sin_addr);
	if (address == NULL || address->sa_len < offset + sizeof(struct in_addr)) {
		return false;
	}
	struct in_addr mask;
	memcpy(&mask, reinterpret_cast<const char*>(address) + offset,
		sizeof(mask));
	return mask.s_addr == 0xffffffffU;
}

bool list_system_routes(std::vector<system_route_entry>& routes,
	acl::string& error)
{
	int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
	size_t required = 0;
	if (sysctl(mib, 6, NULL, &required, NULL, 0) == -1) {
		set_error(error, "get system route dump size", errno);
		return false;
	}

	std::vector<char> buffer(required);
	if (required > 0
		&& sysctl(mib, 6, &buffer[0], &required, NULL, 0) == -1) {
		set_error(error, "read system route dump", errno);
		return false;
	}

	const char* cursor = required > 0 ? &buffer[0] : NULL;
	const char* end = cursor == NULL ? NULL : cursor + required;
	while (cursor != NULL
		&& cursor + sizeof(struct rt_msghdr) <= end) {
		const struct rt_msghdr* header =
			reinterpret_cast<const struct rt_msghdr*>(cursor);
		if (header->rtm_msglen < sizeof(struct rt_msghdr)
			|| cursor + header->rtm_msglen > end) {
			error = "system route dump contains an invalid message";
			return false;
		}
		const int required_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
		if ((header->rtm_flags & required_flags) == required_flags) {
			const char* address_cursor = reinterpret_cast<const char*>(header + 1);
			const char* message_end = cursor + header->rtm_msglen;
			const struct sockaddr_in* destination = NULL;
			const struct sockaddr_in* gateway = NULL;
			const struct sockaddr* netmask = NULL;
			for (unsigned int bit = 1; bit != 0 && address_cursor < message_end;
				bit <<= 1) {
				if ((static_cast<unsigned int>(header->rtm_addrs) & bit) == 0) {
					continue;
				}
				const struct sockaddr* address =
					reinterpret_cast<const struct sockaddr*>(address_cursor);
				size_t length = route_address_size(address);
				if (address_cursor + length > message_end) {
					break;
				}
				if (address->sa_family == AF_INET) {
					if (bit == RTA_DST) {
						destination = reinterpret_cast<const struct sockaddr_in*>(
							address);
					} else if (bit == RTA_GATEWAY) {
						gateway = reinterpret_cast<const struct sockaddr_in*>(address);
					}
				}
				if (bit == RTA_NETMASK) {
					netmask = address;
				}
				address_cursor += length;
			}

			if (destination != NULL && gateway != NULL
				&& ((header->rtm_flags & RTF_HOST) != 0
					|| is_ipv4_host_mask(netmask))) {
				acl::string destination_text;
				acl::string gateway_text;
				if (address_text(destination->sin_addr, destination_text)
					&& address_text(gateway->sin_addr, gateway_text)) {
					char device[IF_NAMESIZE] = { 0 };
					if (header->rtm_index > 0) {
						if_indextoname(header->rtm_index, device);
					}
					routes.push_back(system_route_entry(
						destination_text.c_str(), gateway_text.c_str(), device));
				}
			}
		}
		cursor += header->rtm_msglen;
	}
	return true;
}

#else

bool change_route(int, const char*, const char*, acl::string& error)
{
	error = "route management is unsupported on this operating system";
	return false;
}

bool list_system_routes(std::vector<system_route_entry>&,
	acl::string& error)
{
	error = "system route listing is unsupported on this operating system";
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

bool add_system_route(const char* destination, const char* gateway,
	acl::string& error)
{
#if defined(__linux__)
	return change_route(RTM_NEWROUTE, destination, gateway, error);
#elif defined(__APPLE__) || defined(__FreeBSD__)
	return change_route(RTM_ADD, destination, gateway, error);
#else
	return change_route(0, destination, gateway, error);
#endif
}

bool valid_saved_key(const std::string& key)
{
	if (key.empty() || key.size() > 256) {
		return false;
	}
	for (std::string::const_iterator it = key.begin(); it != key.end(); ++it) {
		unsigned char ch = static_cast<unsigned char>(*it);
		if (ch < 0x20 || ch == 0x7f) {
			return false;
		}
	}
	return true;
}

bool parse_saved_number(const std::string& value, long long& number)
{
	errno = 0;
	char* end = NULL;
	number = strtoll(value.c_str(), &end, 10);
	return errno == 0 && end != value.c_str() && *end == 0;
}

struct saved_route {
	std::string key;
	std::string ip;
	std::string gateway;
	long long ttl;
	long long expires_at;
};

bool load_routes_locked(acl::string& error)
{
	std::ifstream input(routes_file, std::ios::in | std::ios::binary);
	if (!input.is_open()) {
		if (errno == ENOENT) {
			return true;
		}
		set_error(error, "open route database", errno);
		return false;
	}

	std::vector<saved_route> saved;
	std::string line;
	size_t line_number = 0;
	while (std::getline(input, line)) {
		++line_number;
		if (line_number == 1 && line == "# ip-router routes v1") {
			continue;
		}
		if (line.empty()) {
			continue;
		}
		std::vector<std::string> fields;
		std::string::size_type begin = 0;
		for (;;) {
			std::string::size_type separator = line.find('\t', begin);
			fields.push_back(line.substr(begin, separator == std::string::npos
				? std::string::npos : separator - begin));
			if (separator == std::string::npos) {
				break;
			}
			begin = separator + 1;
		}
		long long ttl = 0;
		long long expires_at = 0;
		if (fields.size() != 5 || !valid_saved_key(fields[0])
			|| !route_manager::valid_ipv4(fields[1].c_str())
			|| !route_manager::valid_ipv4(fields[2].c_str())
			|| !parse_saved_number(fields[3], ttl)
			|| !parse_saved_number(fields[4], expires_at)
			|| ttl < 0 || expires_at < 0) {
			logger_error("ignore invalid route database record at line %lu",
				static_cast<unsigned long>(line_number));
			continue;
		}
		saved_route record;
		record.key = fields[0];
		record.ip = fields[1];
		record.gateway = fields[2];
		record.ttl = ttl;
		record.expires_at = expires_at;
		saved.push_back(record);
	}
	if (!input.eof() && input.fail()) {
		error = "read route database failed";
		return false;
	}

	time_t now = time(NULL);
	std::map<std::string, std::string> active_system_routes;
	std::set<std::string> expired_system_routes;
	bool restore_failed = false;
	for (std::vector<saved_route>::const_iterator it = saved.begin();
		it != saved.end(); ++it) {
		std::string route_id = it->ip + "\n" + it->gateway;
		if (it->expires_at > 0
			&& it->expires_at <= static_cast<long long>(now)) {
			expired_system_routes.insert(route_id);
			continue;
		}
		std::map<std::string, std::string>::const_iterator active
			= active_system_routes.find(it->ip);
		if (active != active_system_routes.end()
			&& active->second != it->gateway) {
			logger_error("ignore conflicting persisted route, key=%s, ip=%s, "
				"gateway=%s, active_gateway=%s", it->key.c_str(),
				it->ip.c_str(), it->gateway.c_str(), active->second.c_str());
			continue;
		}
		if (active == active_system_routes.end()) {
			acl::string system_error;
			if (!add_system_route(it->ip.c_str(), it->gateway.c_str(),
				system_error)) {
				logger_error("restore persisted route failed, key=%s, ip=%s, "
					"gateway=%s, error=%s", it->key.c_str(), it->ip.c_str(),
					it->gateway.c_str(), system_error.c_str());
				restore_failed = true;
				continue;
			}
			active_system_routes[it->ip] = it->gateway;
		}
		installed_routes[it->key.c_str()][it->ip.c_str()] = route_metadata(
			it->gateway.c_str(), it->ttl,
			static_cast<time_t>(it->expires_at));
		logger("restored persisted route, key=%s, ip=%s, gateway=%s, "
			"ttl=%lld, expires_at=%lld", it->key.c_str(), it->ip.c_str(),
			it->gateway.c_str(), it->ttl, it->expires_at);
	}

	for (std::set<std::string>::const_iterator it = expired_system_routes.begin();
		it != expired_system_routes.end(); ++it) {
		std::string::size_type separator = it->find('\n');
		std::string destination = it->substr(0, separator);
		if (active_system_routes.find(destination)
			!= active_system_routes.end()) {
			continue;
		}
		std::string gateway = it->substr(separator + 1);
		acl::string system_error;
		if (!delete_system_route(destination.c_str(), gateway.c_str(),
			system_error)) {
			logger_error("delete expired persisted route failed, ip=%s, "
				"gateway=%s, error=%s", destination.c_str(), gateway.c_str(),
				system_error.c_str());
		}
	}

	// 系统路由恢复失败时保留原数据库，避免因为一次临时权限或网络问题
	// 永久丢失持久化记录，等待下次启动时再次尝试恢复。
	if (restore_failed) {
		error = "one or more persisted routes could not be restored";
		return false;
	}
	// 重写数据库以移除过期、无效或冲突的记录。
	return save_routes_locked(error);
}

void expire_routes(void)
{
	std::unique_lock<std::mutex> lock(routes_mutex);
	while (!worker_stopping) {
		time_t now = time(NULL);
		bool changed = false;
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
					changed = true;
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
		if (changed) {
			acl::string persist_error;
			if (!save_routes_locked(persist_error)) {
				logger_error("persist routes after expiration failed, error=%s",
					persist_error.c_str());
			}
		}

		routes_changed.wait_for(lock, std::chrono::seconds(1), [] {
			return worker_stopping;
		});
	}
}

} // namespace

void route_manager::set_storage_path(const char* path)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	if (worker_started) {
		logger_error("cannot change route persistence file after startup");
		return;
	}
	routes_file = path != NULL && *path != 0 ? path : "routes.db";
	global_route_file = routes_file + ".global";
}

void route_manager::start(void)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	if (worker_started) {
		return;
	}
	worker_started = true;
	worker_stopping = false;
	installed_routes.clear();
	global_gateway = "";
	force_global_gateway = false;
	logger("route persistence file=%s", routes_file.c_str());
	logger("global route settings file=%s", global_route_file.c_str());
	acl::string error;
	if (!load_global_route_locked(error)) {
		logger_error("load global route settings failed, file=%s, error=%s",
			global_route_file.c_str(), error.c_str());
	}
	error.clear();
	if (!load_routes_locked(error)) {
		logger_error("load persisted routes failed, file=%s, error=%s",
			routes_file.c_str(), error.c_str());
	}
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

void route_manager::get_global_route(acl::string& gateway, bool& force)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	gateway = global_gateway;
	force = force_global_gateway;
}

bool route_manager::set_global_route(const char* gateway, bool force,
	acl::string& error)
{
	if (gateway != NULL && *gateway != 0 && !valid_ipv4(gateway)) {
		error = "global gateway must be a valid IPv4 address";
		return false;
	}
	std::lock_guard<std::mutex> guard(routes_mutex);
	acl::string previous_gateway = global_gateway;
	bool previous_force = force_global_gateway;
	global_gateway = gateway != NULL ? gateway : "";
	force_global_gateway = !global_gateway.empty() && force;
	if (!save_global_route_locked(error)) {
		global_gateway = previous_gateway;
		force_global_gateway = previous_force;
		return false;
	}
	logger("global route settings updated, gateway=%s, force=%s",
		global_gateway.empty() ? "(disabled)" : global_gateway.c_str(),
		force_global_gateway ? "yes" : "no");
	return true;
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
	const std::map<acl::string, key_routes> previous_routes = installed_routes;
	// 先检查当前 KEY 下是否已经保存了该目标 IP，避免重复操作系统路由表。
	std::map<acl::string, key_routes>::iterator existing_group
		= installed_routes.find(key);
	if (existing_group != installed_routes.end()) {
		key_routes::iterator existing
			= existing_group->second.find(destination);
		// 相同 KEY、IP 和网关的重复添加视为幂等更新，只刷新 TTL 和过期时间。
		if (existing != existing_group->second.end()
			&& existing->second.gateway == gateway) {
			existing->second = route_metadata(gateway,
				ttl > 0 ? ttl : 0, expires_at);
			acl::string persist_error;
			if (!save_routes_locked(persist_error)) {
				installed_routes = previous_routes;
				error = persist_error;
				return false;
			}
			routes_changed.notify_all();
			return true;
		}
	}
	// 再检查其他 KEY 是否已经引用该 IP，以支持多个 KEY 共享同一条系统路由。
	for (std::map<acl::string, key_routes>::const_iterator group
		= installed_routes.begin(); group != installed_routes.end(); ++group) {
		key_routes::const_iterator existing = group->second.find(destination);
		if (existing == group->second.end()) {
			continue;
		}
		// 同一目标 IP 只能对应一个实际网关，否则内存索引会与系统路由不一致。
		if (existing->second.gateway != gateway) {
			error.format("IP %s already uses gateway %s under key %s",
				destination, existing->second.gateway.c_str(),
				group->first.c_str());
			return false;
		}
		// 网关相同时仅增加当前 KEY 的引用，无需重复添加系统路由。
		installed_routes[key][destination]
			= route_metadata(gateway, ttl > 0 ? ttl : 0, expires_at);
		acl::string persist_error;
		if (!save_routes_locked(persist_error)) {
			installed_routes = previous_routes;
			error = persist_error;
			return false;
		}
		routes_changed.notify_all();
		return true;
	}
	// 新路由先持久化，再写入系统路由表；系统操作失败时恢复旧数据库，
	// 保证重启后不会加载一次未成功完成的添加操作。
	installed_routes[key][destination]
		= route_metadata(gateway, ttl > 0 ? ttl : 0, expires_at);
	acl::string persist_error;
	if (!save_routes_locked(persist_error)) {
		installed_routes = previous_routes;
		error = persist_error;
		return false;
	}
	acl::string system_error;
	if (!add_system_route(destination, gateway, system_error)) {
		installed_routes = previous_routes;
		acl::string rollback_error;
		if (!save_routes_locked(rollback_error)) {
			logger_error("rollback route database failed after system add error, "
				"error=%s", rollback_error.c_str());
		}
		error = system_error;
		return false;
	}
	routes_changed.notify_all();
	return true;
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
	const std::map<acl::string, key_routes> previous_routes = installed_routes;

	// 从内存路由列表删除时，必须同时删除对应的系统路由。由于一条系统
	// 主机路由可能被多个 KEY 引用，系统路由删除成功后需要清理所有 KEY
	// 中相同 IP 和网关的引用，避免留下已经失效的内存记录。
	if (!delete_system_route(destination, gateway, error)) {
		return false;
	}
	for (std::map<acl::string, key_routes>::iterator it
		= installed_routes.begin(); it != installed_routes.end();) {
		key_routes::iterator referenced = it->second.find(destination);
		if (referenced != it->second.end()
			&& referenced->second.gateway == gateway) {
			it->second.erase(referenced);
		}
		if (it->second.empty()) {
			it = installed_routes.erase(it);
		} else {
			++it;
		}
	}
	acl::string persist_error;
	if (!save_routes_locked(persist_error)) {
		installed_routes = previous_routes;
		acl::string rollback_error;
		if (!add_system_route(destination, gateway, rollback_error)) {
			logger_error("restore system route failed after persistence error, "
				"ip=%s, gateway=%s, error=%s", destination, gateway,
				rollback_error.c_str());
		}
		error = persist_error;
		return false;
	}
	routes_changed.notify_all();
	return true;
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

bool route_manager::list_system(std::vector<system_route_entry>& routes,
	acl::string& error)
{
	routes.clear();
	error.clear();
	return list_system_routes(routes, error);
}

bool route_manager::remove_system(const char* destination,
	const char* gateway, acl::string& error)
{
	std::lock_guard<std::mutex> guard(routes_mutex);
	std::vector<system_route_entry> system_routes;
	if (!list_system_routes(system_routes, error)) {
		return false;
	}
	bool found = false;
	for (std::vector<system_route_entry>::const_iterator it
		= system_routes.begin(); it != system_routes.end(); ++it) {
		if (it->ip == destination && it->gateway == gateway) {
			found = true;
			break;
		}
	}
	if (!found) {
		error.format("system static host route not found: %s via %s",
			destination, gateway);
		return false;
	}
	const std::map<acl::string, key_routes> previous_routes = installed_routes;

	if (!delete_system_route(destination, gateway, error)) {
		return false;
	}

	// 系统路由已经删除时，同步移除所有 KEY 对该 IP/网关的内存引用，
	// 避免内存列表继续展示一条实际上已经不存在的系统路由。
	for (std::map<acl::string, key_routes>::iterator group
		= installed_routes.begin(); group != installed_routes.end();) {
		key_routes::iterator target = group->second.find(destination);
		if (target != group->second.end() && target->second.gateway == gateway) {
			group->second.erase(target);
		}
		if (group->second.empty()) {
			group = installed_routes.erase(group);
		} else {
			++group;
		}
	}
	acl::string persist_error;
	if (!save_routes_locked(persist_error)) {
		installed_routes = previous_routes;
		acl::string rollback_error;
		if (!add_system_route(destination, gateway, rollback_error)) {
			logger_error("restore system route failed after persistence error, "
				"ip=%s, gateway=%s, error=%s", destination, gateway,
				rollback_error.c_str());
		}
		error = persist_error;
		return false;
	}
	routes_changed.notify_all();
	return true;
}
