#pragma once

//////////////////////////////////////////////////////////////////////////////
// 配置内容项

extern char *var_cfg_upstream_addr;
extern acl::master_str_tbl var_conf_str_tab[];

extern acl::master_bool_tbl var_conf_bool_tab[];

extern int  var_cfg_upstream_timeout;
extern acl::master_int_tbl var_conf_int_tab[];

extern acl::master_int64_tbl var_conf_int64_tab[];

//////////////////////////////////////////////////////////////////////////////

//class acl::socket_stream;

class master_service : public acl::master_udp
{
public:
	master_service(void);
	~master_service(void);

protected:
	// @override
	void on_read(acl::socket_stream* stream);

	// @override
	void thread_on_init(void);

	// @override
	void proc_on_bind(acl::socket_stream& stream);

	// @override
	void proc_on_init(void);

	// @override
	void proc_on_exit(void);

	// @override
	bool proc_on_sighup(acl::string&);
};
