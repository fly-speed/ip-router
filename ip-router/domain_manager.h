#pragma once

class domain_entry {
public:
	domain_entry(const char* name, long long timestamp)
	: domain(name), created_at(timestamp) {}

	acl::string domain;
	long long created_at;
};

class domain_manager {
public:
	static void configure(const char* directory, const char* nameserver,
		int port, int search_order);
	static bool valid_domain(const char* value, acl::string& normalized,
		acl::string& error);
	static bool add(const char* domain, acl::string& error);
	static bool remove(const char* domain, acl::string& error);
	static bool list(std::vector<domain_entry>& domains, acl::string& error);
	static bool refresh_system(acl::string& error);
};
