# ip-router

#### 介绍
ip router，快捷访问网络。

#### 软件架构
软件架构说明


#### 安装教程

1.  xxxx
2.  xxxx
3.  xxxx

#### 使用说明

`ip-router` 提供以下 HTTP 接口（服务默认只监听本机地址）：

- `GET /health`：健康检查。
- `GET /`：显示路由与指定域名 DNS 分流管理页面。
- `GET /domains`：列出本服务管理的全部 DNS 分流域名。
- `POST /domain?domains=<域名列表>`：批量添加 DNS 分流域名，支持用逗号、分号或空白分隔，一次最多 4096 个。
- `DELETE /domain?domain=<域名>`：删除一个 DNS 分流域名；也可使用 `domains` 参数批量删除。
- `GET /routes`：以字符串 `key` 分组，列出本服务成功设置且尚未删除的全部 IP 路由。
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
修改系统路由表通常需要以 root 或具备相应网络管理权限的用户运行服务。

在 macOS 上，域名管理接口会为每个域名创建
`resolver_dir/<域名>`，使该域名及其全部子域名使用 dns-gate，其余域名继续使用
系统或 VPN 的默认 DNS。默认配置为：

```ini
resolver_dir = /etc/resolver
dns_gate_nameserver = 127.0.0.1
dns_gate_port = 53
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

服务将内存路由持久化到配置项 `routes_file` 指定的文件。该配置留空或未设置时，默认使用程序当前运行目录下的 `routes.db`；既可填写绝对路径，也可填写相对于当前运行目录的路径（目标目录需已存在且可写）。启动时会恢复其中尚未过期的路由；添加、删除及 TTL 自动过期时会同步更新该文件。通过 ACL master 部署且使用默认配置时，文件位于 `{install_path}/var/routes.db`。

根页面模板位于 `ip-router/html/index.html`。服务会在每次请求时读取模板，页面通过 `/routes` 接口加载数据，修改 HTML 后刷新即可生效。
页面中的服务内存路由会先按可注册根域名合并显示；例如
`www.iqiyi.com` 和
`ipv6-static.dns.iqiyi.com` 会折叠在同一个 `iqiyi.com` 分组下。
服务内存路由表支持用复选框选择一个或多个根域名，并批量写入
`resolver_dir`。macOS 写入完成后会自动刷新 DNS 缓存并通知
`mDNSResponder` 重新加载配置；已经配置的根域名会在页面中标记为“已配置”。

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
ip_router_addr = 127.0.0.1:8088
ip_router_gateway = 192.168.1.1
ip_router_timeout = 3
ip_router_ttl = 600
```

`geoip_countries` 支持用逗号、分号或空白分隔多个 ISO 3166-1 两位国家代码。
`ip_router_gateway` 必须按实际网络环境设置，留空时关闭 GeoIP 自动路由。
MMDB 查询或路由请求失败时服务会记录日志并继续返回 DNS 响应。[DB-IP Lite](https://db-ip.com/db/lite.php)
数据采用 CC BY 4.0 许可证，发布使用结果时需按其许可要求注明数据来源。

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
