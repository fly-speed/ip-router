#include "stdafx.h"
#include "dgate_service.h"
#include "master_service.h"

//////////////////////////////////////////////////////////////////////////////
// 配置内容项

char *var_cfg_upstream_addr;
acl::master_str_tbl var_conf_str_tab[] = {
	{ "upstream_addr", "114.114.114.114|53", &var_cfg_upstream_addr },

	{ 0, 0, 0 }
};

acl::master_bool_tbl var_conf_bool_tab[] = {
	{ 0, 0, 0 }
};

int  var_cfg_upstream_timeout;
acl::master_int_tbl var_conf_int_tab[] = {
	{ "upstream_timeout", 5, &var_cfg_upstream_timeout, 1, 300 },

	{ 0, 0 , 0 , 0, 0 }
};

acl::master_int64_tbl var_conf_int64_tab[] = {
	{ 0, 0 , 0 , 0, 0 }
};

//////////////////////////////////////////////////////////////////////////////

master_service::master_service(void)
{
}

master_service::~master_service(void)
{
}

void master_service::on_read(acl::socket_stream* stream)
{
	char buf[65536];
	int n = stream->read(buf, sizeof(buf), false);

	if (n == -1) {
		return;
	}

	dgate_push_request(stream, stream->get_peer(true), buf,
		static_cast<size_t>(n));
}

void master_service::thread_on_init(void)
{
	logger(">>thread_on_init<<<");
}

void master_service::proc_on_bind(acl::socket_stream&)
{
	logger(">>>proc_on_bind<<<");
}

void master_service::proc_on_init(void)
{
	logger("DNS proxy upstream=%s, timeout=%d seconds",
		var_cfg_upstream_addr, var_cfg_upstream_timeout);
	dgate_service_start();
}

void master_service::proc_on_exit(void)
{
	logger(">>>proc_on_exit<<<");
}

bool master_service::proc_on_sighup(acl::string&)
{
	logger(">>>proc_on_sighup<<<");
	return true;
}
