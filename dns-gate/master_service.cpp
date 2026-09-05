#include "stdafx.h"
#include "dgate_service.h"
#include "geoip_router.h"
#include "master_service.h"

//////////////////////////////////////////////////////////////////////////////
// 配置内容项

char *var_cfg_upstream_addr;
char *var_cfg_geoip_database;
char *var_cfg_geoip_countries;
char *var_cfg_ip_router_addr;
char *var_cfg_ip_router_gateway;
acl::master_str_tbl var_conf_str_tab[] = {
	{ "upstream_addr", "114.114.114.114|53", &var_cfg_upstream_addr },
	{ "geoip_database", "dbip-country-lite.mmdb", &var_cfg_geoip_database },
	{ "geoip_countries", "CN", &var_cfg_geoip_countries },
	{ "ip_router_addr", "127.0.0.1:8888", &var_cfg_ip_router_addr },
	{ "ip_router_gateway", "192.168.1.1", &var_cfg_ip_router_gateway },

	{ 0, 0, 0 }
};

acl::master_bool_tbl var_conf_bool_tab[] = {
	{ 0, 0, 0 }
};

int  var_cfg_upstream_timeout;
int  var_cfg_ip_router_timeout;
int  var_cfg_ip_router_ttl;
acl::master_int_tbl var_conf_int_tab[] = {
	{ "upstream_timeout", 5, &var_cfg_upstream_timeout, 1, 300 },
	{ "ip_router_timeout", 3, &var_cfg_ip_router_timeout, 1, 300 },
	{ "ip_router_ttl", 600, &var_cfg_ip_router_ttl, 0, 2147483647 },

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
	geoip_router_start(var_cfg_geoip_database, var_cfg_geoip_countries,
		var_cfg_ip_router_addr, var_cfg_ip_router_gateway,
		var_cfg_ip_router_timeout, var_cfg_ip_router_ttl);
	dgate_service_start();
}

void master_service::proc_on_exit(void)
{
	logger(">>>proc_on_exit<<<");
	geoip_router_stop();
}

bool master_service::proc_on_sighup(acl::string&)
{
	logger(">>>proc_on_sighup<<<");
	return true;
}
