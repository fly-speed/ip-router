#pragma once

class route_target {
public:
	route_target(const char* destination, const char* next_hop,
		long long configured_ttl, long long expiration_time)
	: ip(destination), gateway(next_hop), ttl(configured_ttl),
	  expires_at(expiration_time) {}

	acl::string ip;
	acl::string gateway;
	long long ttl;
	long long expires_at;
};

class route_entry {
public:
	explicit route_entry(const char* name) : key(name) {}

	acl::string key;
	std::vector<route_target> targets;
};

class system_route_entry {
public:
	system_route_entry(const char* destination, const char* next_hop,
		const char* device)
	: ip(destination), gateway(next_hop), interface_name(device) {}

	acl::string ip;
	acl::string gateway;
	acl::string interface_name;
};

class route_manager {
public:
	static void set_storage_path(const char* path);
	static void start(void);
	static void stop(void);
	static bool valid_ipv4(const char* value);
	static void get_global_route(acl::string& gateway, bool& force);
	static bool set_global_route(const char* gateway, bool force,
		acl::string& error);
	static bool add(const char* destination, const char* gateway,
		const char* key, long long ttl, acl::string& error);
	static bool remove(const char* key, const char* destination,
		const char* gateway, acl::string& error);
	static void list(std::vector<route_entry>& routes);
	static bool list_system(std::vector<system_route_entry>& routes,
		acl::string& error);
	static bool remove_system(const char* destination, const char* gateway,
		acl::string& error);
};
