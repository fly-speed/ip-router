#include "stdafx.h"
#include "master_service.h"

int main(int argc, char* argv[])
{
	acl::acl_cpp_init();

	master_service& ms = acl::singleton2<master_service>::get_instance();

	ms.set_cfg_int(var_conf_int_tab);
	ms.set_cfg_int64(var_conf_int64_tab);
	ms.set_cfg_str(var_conf_str_tab);
	ms.set_cfg_bool(var_conf_bool_tab);

	if (argc == 1 || (argc >= 2 && strcmp(argv[1], "alone") == 0)) {
		acl::log::stdout_open(true);
		acl::master_log_enable(false);

		const char* addrs = "127.0.0.1|53";
		printf("bind on: %s\r\n", addrs);

		unsigned int count = 0;

		if (argc >= 3) {
			ms.run_alone(addrs, argv[2], count);
		} else {
			ms.run_alone(addrs, NULL, count);
		}

		printf("Enter any key to exit now\r\n");
		getchar();
	} else {
#if defined(_WIN32) || defined(_WIN64)
		acl::log::stdout_open(true);

		const char* addrs = "127.0.0.1:53";
		printf("bind on: %s\r\n", addrs);

		unsigned int count = 0;

		ms.run_alone(addrs, NULL, count);
		printf("Enter any key to exit now\r\n");
		getchar();
#else
		ms.run_daemon(argc, argv);
#endif
	}

	return 0;
}
