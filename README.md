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
- `GET /`：显示路由管理 HTML 页面，可添加、批量删除及逐条删除路由。
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

根页面模板位于 `ip-router/html/index.html`。服务会在每次请求时读取模板，页面通过 `/routes` 接口加载数据，修改 HTML 后刷新即可生效。

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
