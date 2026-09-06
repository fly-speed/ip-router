# ip-router

#### 介绍
ip router，快捷访问网络。

#### 软件架构
软件架构说明


#### 安装教程

项目在 `ip-router/package` 中提供平台原生安装包构建脚本。运行安装包前，
需要先将 acl-master 安装至 `/opt/soft/acl-master`。

macOS：

```shell
cd ip-router/package
./build-macos.sh
sudo installer -pkg ./dist/ip-router-1.0.0-macos-$(uname -m).pkg -target /
```

Ubuntu：

```shell
cd ip-router/package
./build-ubuntu.sh
sudo apt install ./dist/ip-router_1.0.0_$(dpkg --print-architecture).deb
```

dns-gate 使用对应目录下的脚本：

```shell
# macOS
cd dns-gate/package
./build-macos.sh

# Ubuntu
cd dns-gate/package
./build-ubuntu.sh
```

ip-router 和 dns-gate 的安装路径分别固定为 `/opt/soft/ip-router` 和
`/opt/soft/dns-gate`。安装程序会自动将服务加入
`/opt/soft/acl-master/conf/services.cf` 并启动服务。详细的版本、签名、
升级和构建参数参见两个模块各自的 `package/README.md`。

#### 使用说明

`ip-router` 提供以下 HTTP 接口（服务默认只监听本机地址）：

- `GET /health`：健康检查。
- `GET /`：显示路由与指定域名 DNS 分流管理页面。
- `GET /domains`：列出本服务管理的全部 DNS 分流域名。
- `GET /tlds`：列出随项目提供的 IANA 根区一级域名及清单版本。
- `POST /domain?domains=<域名列表>`：批量添加 DNS 分流域名，支持用逗号、分号或空白分隔，一次最多 4096 个。
- `DELETE /domain?domain=<域名>`：删除一个 DNS 分流域名；也可使用 `domains` 参数批量删除。
- `GET /routes`：以字符串 `key` 分组，列出本服务成功设置且尚未删除的全部 IP 路由。
- `GET /route-settings`：读取全局网关及强制使用状态。
- `POST /route-settings?gateway=<网关IPv4>&force=<布尔值>`：保存全局网关；`force` 可选，支持 `0/1`、`true/false`、`yes/no` 和 `on/off`。
- `DELETE /route-settings`：取消全局网关设置。
- `GET /system-routes`：直接读取系统路由表，列出静态 IPv4 主机路由及其网关和网络接口。
- `DELETE /system-route?ip=<目标IPv4>&gateway=<网关IPv4>`：直接删除指定的系统静态主机路由。
- `POST /route?ip=<目标IPv4>&gateway=<网关IPv4>&key=<字符串KEY>&ttl=<秒>`：向指定 KEY 添加或替换一个目标 IP 的主机路由。
- `POST /route?ips=<IPv4列表>&gateway=<网关IPv4>&key=<字符串KEY>&ttl=<秒>`：为同一 KEY 批量添加多个 IP，IP 之间用逗号、分号或空白分隔。
- `DELETE /route?key=<字符串KEY>`：删除指定 KEY 下的全部 IP 路由。
- `DELETE /route?ip=<目标IPv4>`：删除所有 KEY 下匹配该目标 IP 的路由。
- `DELETE /route?key=<字符串KEY>&ip=<目标IPv4>`：删除指定 KEY 下匹配该目标 IP 的路由。

参数 `target` 可作为 `ip` 的别名，参数 `route` 可作为 `gateway` 的别名。
旧的 `domain` 参数暂时可作为 `key` 的兼容别名。
`ttl` 为可选的整数秒数；缺省或小于等于 `0` 时永不过期，大于 `0` 时到期自动删除系统路由。
添加路由时，如果请求没有 `gateway`/`route` 参数，则使用已配置的全局网关；
开启全局网关的 `force` 后，即使请求指定了网关也会被全局网关覆盖。
全局网关设置持久化在路由数据库同目录的 `<routes_file>.global` 文件中，重启后自动加载。
修改系统路由表通常需要以 root 或具备相应网络管理权限的用户运行服务。

在 macOS 上，域名管理接口会为每个域名创建
`resolver_dir/<域名>`，使该域名及其全部子域名使用 dns-gate，其余域名继续使用
系统或 VPN 的默认 DNS。默认配置为：

```ini
resolver_dir = /etc/resolver
dns_gate_nameserver = 127.0.0.1
dns_gate_port = 53
dns_gate_http_addr = 127.0.0.1:8053
dns_gate_http_timeout = 8
resolver_search_order = 1
```

服务需要具备创建及删除 `resolver_dir` 中文件的权限，通常应以 root 运行。
接口只会更新或删除带有 `# managed by ip-router` 标记的文件；如果同名文件由
用户或其他软件创建，添加和删除操作都会拒绝执行，避免破坏已有 DNS 配置。

例如：

```shell
curl -X POST --data-urlencode $'domains=webcool.cn\nexample.com' \
  'http://127.0.0.1:8088/domain'
curl 'http://127.0.0.1:8088/domains'
curl -X DELETE 'http://127.0.0.1:8088/domain?domain=webcool.cn'
```

服务将内存路由持久化到配置项 `routes_file` 指定的文件。该配置留空或未设置时，默认使用程序当前运行目录下的 `routes.db`；既可填写绝对路径，也可填写相对于当前运行目录的路径（目标目录需已存在且可写）。启动时会恢复其中尚未过期的路由；添加、删除及 TTL 自动过期时会同步更新该文件。通过安装包部署时，文件位于 `/opt/soft/ip-router/conf/routes.db`。

根页面模板位于 `ip-router/html/index.html`。服务会在每次请求时读取模板，页面通过 `/routes` 接口加载数据，修改 HTML 后刷新即可生效。
管理页面采用左右分栏布局：左侧为固定功能导航，点击后在右侧切换并只显示
对应的功能区；窄屏设备会自动切换为顶部横向导航。
页面中的服务内存路由会先按可注册根域名合并显示；例如
`www.iqiyi.com` 和
`ipv6-static.dns.iqiyi.com` 会折叠在同一个 `iqiyi.com` 分组下。
服务内存路由表支持用复选框选择一个或多个根域名，并批量写入
`resolver_dir`。macOS 写入完成后会自动刷新 DNS 缓存并通知
`mDNSResponder` 重新加载配置；已经配置的根域名会在页面中标记为“已配置”。

“全局 DNS 接管”区域使用 IANA 官方根区 TLD 清单，可以搜索、选择部分
TLD，或一键将全部尚未配置的 TLD 写入 `resolver_dir`。这样可以让 dns-gate
接管所有公共 DNS 根区域名；私有后缀、单标签主机名以及 mDNS 名称不属于
IANA 根区，因此不在该功能覆盖范围内。当前清单位于
`ip-router/html/tlds-alpha-by-domain.txt`，更新方式：

```shell
cd ip-router
./update-tlds.sh
```

清单来源：<https://data.iana.org/TLD/tlds-alpha-by-domain.txt>。

#### dns-gate GeoIP 自动路由

`dns-gate` 集成了项目根目录 `vendor/libmaxminddb` 中的 libmaxminddb
1.13.3 源码，不依赖系统预装该库。收到上游 DNS 响应后，服务查询 A
记录对应的国家代码；命中 `geoip_countries` 时，会先调用 `ip-router` 的
批量添加接口，调用完成后再把原始 DNS 响应返回客户端。

DB-IP Country Lite 数据文件不包含在源码仓库中。可在 `dns-gate` 目录运行：

```shell
./update-dbip.sh
```

也可以指定输出路径和数据库月份：

```shell
./update-dbip.sh /var/lib/ip-router/dbip-country-lite.mmdb 2026-09
```

然后在 `dns-gate.cf` 中配置：

```ini
geoip_database = /var/lib/ip-router/dbip-country-lite.mmdb
geoip_countries = CN
http_addr = 127.0.0.1:8053
ip_router_addr = 127.0.0.1:8088
ip_router_gateway = 192.168.1.1
ip_router_timeout = 3
ip_router_ttl = 600
```

`geoip_countries` 支持用逗号、分号或空白分隔多个 ISO 3166-1 两位国家代码。
`ip_router_gateway` 必须按实际网络环境设置，留空时关闭 GeoIP 自动路由。
MMDB 查询或路由请求失败时服务会记录日志并继续返回 DNS 响应。[DB-IP Lite](https://db-ip.com/db/lite.php)
数据采用 CC BY 4.0 许可证，发布使用结果时需按其许可要求注明数据来源。

dns-gate 同时在 `http_addr` 提供只读诊断接口：

```shell
curl 'http://127.0.0.1:8053/lookup?domain=weibo.com'
```

接口使用与 UDP 代理相同的上游 DNS 和 GeoIP 数据库，返回 A 记录、CNAME、
国家代码以及每个 IP 是否命中 `geoip_countries`。诊断查询不会向 ip-router
添加路由。ip-router 的 `GET /dns-lookup?domain=<域名>` 会代理该接口，管理
页面左侧的“域名归属诊断”可直接展示结果。出于安全考虑，建议两个 HTTP
服务均只监听本机地址。

国家数据库加载状态与自动路由状态相互独立：即使未配置网关，诊断接口仍可
查询 IP 国家；只有 `geoip_countries`、`ip_router_addr` 和
`ip_router_gateway` 均有效时，自动路由状态才会显示为“已就绪”。

`GET /routes` 按字符串 KEY 分组返回：

```json
{
  "success": true,
  "count": 1,
  "routes": [
    {
      "key": "dns.google",
      "count": 2,
      "ips": [
        {"ip": "8.8.8.8", "gateway": "192.168.1.1", "ttl": 300, "expires_at": 1788574800},
        {"ip": "8.8.4.4", "gateway": "192.168.1.1", "ttl": 300, "expires_at": 1788574800}
      ]
    }
  ]
}
```

例如：

```shell
curl -X POST 'http://127.0.0.1:8088/route?ip=8.8.8.8&gateway=192.168.1.1&key=dns.google&ttl=300'
curl -X POST 'http://127.0.0.1:8088/route?ips=8.8.8.8,8.8.4.4&gateway=192.168.1.1&key=dns.google&ttl=300'
curl -X POST 'http://127.0.0.1:8088/route-settings?gateway=192.168.1.1&force=1'
curl -X POST 'http://127.0.0.1:8088/route?ip=8.8.8.8&key=dns.google&ttl=300'
curl -X DELETE 'http://127.0.0.1:8088/route-settings'
curl -X DELETE 'http://127.0.0.1:8088/route?key=dns.google'
curl -X DELETE 'http://127.0.0.1:8088/route?ip=8.8.8.8'
curl -X DELETE 'http://127.0.0.1:8088/route?key=dns.google&ip=8.8.8.8'
```

#### 参与贡献

1.  Fork 本仓库
2.  新建 Feat_xxx 分支
3.  提交代码
4.  新建 Pull Request


#### 特技

1.  使用 Readme\_XXX.md 来支持不同的语言，例如 Readme\_en.md, Readme\_zh.md
2.  Gitee 官方博客 [blog.gitee.com](https://blog.gitee.com)
3.  你可以 [https://gitee.com/explore](https://gitee.com/explore) 这个地址来了解 Gitee 上的优秀开源项目
4.  [GVP](https://gitee.com/gvp) 全称是 Gitee 最有价值开源项目，是综合评定出的优秀开源项目
5.  Gitee 官方提供的使用手册 [https://gitee.com/help](https://gitee.com/help)
6.  Gitee 封面人物是一档用来展示 Gitee 会员风采的栏目 [https://gitee.com/gitee-stars/](https://gitee.com/gitee-stars/)
