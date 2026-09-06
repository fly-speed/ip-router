#include "stdafx.h"
#include "domain_manager.h"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
# include <spawn.h>
# include <sys/wait.h>
extern char** environ;
#endif

namespace {

const char* managed_header = "# managed by ip-router";
std::mutex domains_mutex;
std::string resolver_directory("/etc/resolver");
std::string resolver_nameserver("127.0.0.1");
int resolver_port = 53;
int resolver_search_order = 1;

void set_error(acl::string& error, const char* operation, int code)
{
	error.format("%s failed: %s (errno=%d)", operation, strerror(code), code);
}

std::string path_join(const std::string& directory, const std::string& name)
{
	if (!directory.empty() && directory[directory.size() - 1] == '/') {
		return directory + name;
	}
	return directory + "/" + name;
}

bool ensure_directory(acl::string& error)
{
	struct stat info;
	if (stat(resolver_directory.c_str(), &info) == 0) {
		if (S_ISDIR(info.st_mode)) {
			return true;
		}
		error.format("resolver path is not a directory: %s",
			resolver_directory.c_str());
		return false;
	}
	if (errno != ENOENT) {
		set_error(error, "stat resolver directory", errno);
		return false;
	}
	if (mkdir(resolver_directory.c_str(), 0755) != 0) {
		set_error(error, "create resolver directory", errno);
		return false;
	}
	return true;
}

bool read_managed_file(const std::string& path, long long& created_at)
{
	std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
	if (!input.is_open()) {
		return false;
	}
	std::string line;
	if (!std::getline(input, line) || line != managed_header) {
		return false;
	}
	created_at = 0;
	if (std::getline(input, line)) {
		const std::string prefix("# created_at=");
		if (line.compare(0, prefix.size(), prefix) == 0) {
			std::istringstream value(line.substr(prefix.size()));
			value >> created_at;
		}
	}
	return true;
}

#if defined(__APPLE__)
bool run_program(const char* path, char* const arguments[], acl::string& error)
{
	pid_t process = 0;
	int code = posix_spawn(&process, path, NULL, NULL, arguments, environ);
	if (code != 0) {
		set_error(error, path, code);
		return false;
	}
	int status = 0;
	while (waitpid(process, &status, 0) == -1) {
		if (errno == EINTR) {
			continue;
		}
		set_error(error, "wait for DNS refresh command", errno);
		return false;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		error.format("DNS refresh command failed: %s (status=%d)", path,
			status);
		return false;
	}
	return true;
}
#endif

} // namespace

void domain_manager::configure(const char* directory, const char* nameserver,
	int port, int search_order)
{
	std::lock_guard<std::mutex> guard(domains_mutex);
	resolver_directory = directory != NULL && *directory != 0
		? directory : "/etc/resolver";
	resolver_nameserver = nameserver != NULL && *nameserver != 0
		? nameserver : "127.0.0.1";
	resolver_port = port > 0 && port <= 65535 ? port : 53;
	resolver_search_order = search_order >= 0 ? search_order : 1;
	logger("domain resolver directory=%s, nameserver=%s, port=%d, "
		"search_order=%d", resolver_directory.c_str(),
		resolver_nameserver.c_str(), resolver_port, resolver_search_order);
}

bool domain_manager::valid_domain(const char* value, acl::string& normalized,
	acl::string& error)
{
	normalized.clear();
	if (value == NULL || *value == 0) {
		error = "domain is required";
		return false;
	}
	std::string domain(value);
	while (!domain.empty() && domain[domain.size() - 1] == '.') {
		domain.erase(domain.size() - 1);
	}
	if (domain.empty() || domain.size() > 253) {
		error = "domain length must be between 1 and 253 characters";
		return false;
	}

	size_t label_length = 0;
	for (size_t i = 0; i < domain.size(); ++i) {
		unsigned char ch = static_cast<unsigned char>(domain[i]);
		if (ch == '.') {
			if (label_length == 0 || label_length > 63
				|| domain[i - 1] == '-') {
				error.format("invalid domain: %s", value);
				return false;
			}
			label_length = 0;
			continue;
		}
		if (!(isalnum(ch) || ch == '-')) {
			error.format("domain must contain only letters, digits, '-' and '.': %s",
				value);
			return false;
		}
		if (label_length == 0 && ch == '-') {
			error.format("domain label cannot start with '-': %s", value);
			return false;
		}
		domain[i] = static_cast<char>(tolower(ch));
		++label_length;
	}
	if (label_length == 0 || label_length > 63
		|| domain[domain.size() - 1] == '-') {
		error.format("invalid domain: %s", value);
		return false;
	}
	normalized = domain.c_str();
	return true;
}

bool domain_manager::add(const char* value, acl::string& error)
{
	acl::string domain;
	if (!valid_domain(value, domain, error)) {
		return false;
	}
	std::lock_guard<std::mutex> guard(domains_mutex);
	if (!ensure_directory(error)) {
		return false;
	}

	const std::string path = path_join(resolver_directory, domain.c_str());
	struct stat info;
	long long created_at = static_cast<long long>(time(NULL));
	if (lstat(path.c_str(), &info) == 0) {
		long long existing_created_at = 0;
		if (!S_ISREG(info.st_mode)
			|| !read_managed_file(path, existing_created_at)) {
			error.format("resolver file already exists and is not managed by "
				"ip-router: %s", path.c_str());
			return false;
		}
		if (existing_created_at > 0) {
			created_at = existing_created_at;
		}
	} else if (errno != ENOENT) {
		set_error(error, "inspect resolver file", errno);
		return false;
	}

	acl::string temporary;
	temporary.format("%s/.ip-router-%ld-%s.tmp", resolver_directory.c_str(),
		static_cast<long>(getpid()), domain.c_str());
	std::ofstream output(temporary.c_str(),
		std::ios::out | std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		set_error(error, "open temporary resolver file", errno);
		return false;
	}
	output << managed_header << '\n'
		<< "# created_at=" << created_at << '\n'
		<< "nameserver " << resolver_nameserver << '\n'
		<< "port " << resolver_port << '\n'
		<< "search_order " << resolver_search_order << '\n';
	output.flush();
	if (!output.good()) {
		output.close();
		::unlink(temporary.c_str());
		error = "write resolver file failed";
		return false;
	}
	output.close();
	if (::rename(temporary.c_str(), path.c_str()) != 0) {
		int code = errno;
		::unlink(temporary.c_str());
		set_error(error, "replace resolver file", code);
		return false;
	}
	return true;
}

bool domain_manager::remove(const char* value, acl::string& error)
{
	acl::string domain;
	if (!valid_domain(value, domain, error)) {
		return false;
	}
	std::lock_guard<std::mutex> guard(domains_mutex);
	const std::string path = path_join(resolver_directory, domain.c_str());
	struct stat info;
	if (lstat(path.c_str(), &info) != 0) {
		if (errno == ENOENT) {
			error.format("domain not found: %s", domain.c_str());
		} else {
			set_error(error, "inspect resolver file", errno);
		}
		return false;
	}
	long long created_at = 0;
	if (!S_ISREG(info.st_mode) || !read_managed_file(path, created_at)) {
		error.format("refusing to delete resolver file not managed by ip-router: %s",
			path.c_str());
		return false;
	}
	if (::unlink(path.c_str()) != 0) {
		set_error(error, "delete resolver file", errno);
		return false;
	}
	return true;
}

bool domain_manager::list(std::vector<domain_entry>& domains, acl::string& error)
{
	std::lock_guard<std::mutex> guard(domains_mutex);
	domains.clear();
	DIR* directory = opendir(resolver_directory.c_str());
	if (directory == NULL) {
		if (errno == ENOENT) {
			return true;
		}
		set_error(error, "open resolver directory", errno);
		return false;
	}

	struct dirent* item;
	while ((item = readdir(directory)) != NULL) {
		if (item->d_name[0] == '.') {
			continue;
		}
		acl::string normalized;
		acl::string validation_error;
		if (!valid_domain(item->d_name, normalized, validation_error)
			|| normalized != item->d_name) {
			continue;
		}
		const std::string path = path_join(resolver_directory, item->d_name);
		struct stat info;
		long long created_at = 0;
		if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)
			|| !read_managed_file(path, created_at)) {
			continue;
		}
		domains.push_back(domain_entry(item->d_name, created_at));
	}
	closedir(directory);
	std::sort(domains.begin(), domains.end(),
		[](const domain_entry& left, const domain_entry& right) {
			return left.domain < right.domain;
		});
	return true;
}

bool domain_manager::refresh_system(acl::string& error)
{
#if defined(__APPLE__)
	char cache_program[] = "/usr/bin/dscacheutil";
	char cache_option[] = "-flushcache";
	char* cache_arguments[] = { cache_program, cache_option, NULL };
	if (!run_program(cache_program, cache_arguments, error)) {
		return false;
	}
	char responder_program[] = "/usr/bin/killall";
	char responder_signal[] = "-HUP";
	char responder_name[] = "mDNSResponder";
	char* responder_arguments[] = {
		responder_program, responder_signal, responder_name, NULL
	};
	return run_program(responder_program, responder_arguments, error);
#else
	(void) error;
	return true;
#endif
}
