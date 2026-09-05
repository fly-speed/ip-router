#include "stdafx.h"
#include "master_service.h"
#include "http_service.h"
#include "route_service.h"

int main(int argc, char *argv[])
{
	acl::acl_cpp_init();

	master_service ms;
	http_service& service = ms.get_service();
	register_route_service(service);

	// setup the configure

	ms.set_cfg_int(var_conf_int_tab)
		.set_cfg_int64(var_conf_int64_tab)
		.set_cfg_str(var_conf_str_tab)
		.set_cfg_bool(var_conf_bool_tab);

	if (argc == 1 || (argc >= 2 && strcasecmp(argv[1], "alone") == 0)) {
		// Log to standard output in standalone mode.
		acl::log::stdout_open(true);
		// Do not create acl_master.log in standalone mode.
		acl::master_log_enable(false);

		const char* addr = "127.0.0.1|8888";

		if (argc >= 4) {
			addr = argv[3];
		}
		printf("listen: %s\r\n", addr);

		ms.run_alone(addr, argc >= 3 ? argv[2] : NULL);
	} else {
#if defined(_WIN32) || defined(_WIN64)
		const char* addr = "127.0.0.1:8887";

		acl::log::stdout_open(true);
		printf("listen: %s\r\n", addr);

		ms.run_alone(addr, NULL);
#else
		ms.run_daemon(argc, argv);
#endif
	}

	return 0;
}
