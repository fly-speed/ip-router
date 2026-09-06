#pragma once

class http_service;

extern char *var_cfg_dns_gate_http_addr;
extern int var_cfg_dns_gate_http_timeout;

class master_service : public acl::master_fiber
{
public:
	master_service(void);
	~master_service(void);

	http_service& get_service(void) const;

protected:
	// @override
	void on_accept(acl::socket_stream& conn);

	// @override
	void proc_pre_jail(void);

	// @override
	void proc_on_listen(acl::server_socket& ss);

	// @override
	void proc_on_init(void);

	// @override
	void proc_on_exit(void);

	// @override
	bool proc_on_sighup(acl::string&);

private:
	acl::sslbase_conf* conf_;
	http_service* service_;

	acl::sslbase_io* setup_ssl(acl::socket_stream& conn,
		acl::sslbase_conf& conf);
};
