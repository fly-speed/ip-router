#ifndef MAXMINDDB_CONFIG_H
#define MAXMINDDB_CONFIG_H

/* Makefile 直接编译 vendor 源码时使用；CMake 会自行探测并生成此配置。 */
#define MMDB_UINT128_USING_MODE 0
#if defined(_MSC_VER)
# define MMDB_UINT128_IS_BYTE_ARRAY 1
#else
# define MMDB_UINT128_IS_BYTE_ARRAY 0
#endif

#endif
