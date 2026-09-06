# dns-gate 安装包

本目录提供两个平台原生安装包构建脚本：

- `build-macos.sh`：在 macOS 上编译并生成 `.pkg`。
- `build-ubuntu.sh`：在 Ubuntu 上编译并生成 `.deb`。

脚本只能在对应的目标平台运行，不进行交叉编译。安装包固定安装至
`/opt/soft/dns-gate`，并将服务配置注册到
`/opt/soft/acl-master/conf/services.cf`。安装完成后会通过 `master_ctl`
启动或重启 dns-gate。

## 前置条件

运行安装包前，acl-master 必须已经安装在：

```text
/opt/soft/acl-master
```

打包前必须准备好 `dns-gate/dbip-country-lite.mmdb`。如果该文件不存在，先在
`dns-gate` 目录执行：

```shell
./update-dbip.sh
```

也可以通过 `DBIP_DATABASE=/path/to/database.mmdb` 指定其他 DB-IP Country
Lite 数据库。构建机器还需要具备本项目原有的 ACL 开发库及编译环境。

## macOS

```shell
cd dns-gate/package
./build-macos.sh
sudo installer -pkg ./dist/dns-gate-1.0.0-macos-$(uname -m).pkg -target /
```

可以指定版本和输出目录：

```shell
./build-macos.sh 1.2.0 /tmp/packages
```

需要签名时设置 Apple Installer 签名身份：

```shell
PKG_SIGN_IDENTITY='Developer ID Installer: Example Corp (TEAMID)' \
  ./build-macos.sh 1.2.0
```

## Ubuntu

```shell
cd dns-gate/package
./build-ubuntu.sh
sudo apt install ./dist/dns-gate_1.0.0_$(dpkg --print-architecture).deb
```

同样可以指定版本和输出目录：

```shell
./build-ubuntu.sh 1.2.0 /tmp/packages
```

## 构建参数

- `DNS_GATE_VERSION`：没有位置参数时覆盖 `VERSION` 文件中的版本。
- `DBIP_DATABASE`：需要打入安装包的 DB-IP Country Lite 数据库。
- `BUILD_JOBS`：并行编译任务数。
- `SKIP_BUILD=1`：跳过编译，直接打包当前平台已有的 `dns-gate` 二进制。
- `PACKAGE_MAINTAINER`：Ubuntu 软件包维护者字段。
- `PKG_SIGN_IDENTITY`：macOS Installer 证书名称。

## 安装后的目录

```text
/opt/soft/dns-gate/
├── bin/dns-gate-service
├── sbin/dns-gate
├── conf/dns-gate.cf
├── conf/dns-gate.cf.default
├── sh/update-dbip.sh
├── share/dbip/dbip-country-lite.mmdb
├── share/doc/DBIP-LICENSE.txt
├── var/dbip-country-lite.mmdb
├── var/log/
└── var/pid/
```

升级时不会覆盖现有的 `conf/dns-gate.cf` 和 `var/dbip-country-lite.mmdb`。
安装包中的数据库副本保存在 `share/dbip`，只在首次安装或运行数据库缺失时复制。
如果需要根据国家代码自动调用 ip-router，请先安装并启动 ip-router，然后在
`conf/dns-gate.cf` 中设置实际的 `ip_router_gateway`、国家代码和服务地址。
默认的 `http_addr = 127.0.0.1:8053` 为 ip-router 管理页面提供域名解析及
GeoIP 诊断 API；如需修改端口，应同步修改 ip-router 配置中的
`dns_gate_http_addr`。

更新数据库并重启服务：

```shell
cd /opt/soft/dns-gate/var
sudo ../sh/update-dbip.sh dbip-country-lite.mmdb
sudo ../bin/dns-gate-service restart
```

其他服务管理命令：

```shell
sudo /opt/soft/dns-gate/bin/dns-gate-service start
sudo /opt/soft/dns-gate/bin/dns-gate-service stop
sudo /opt/soft/dns-gate/bin/dns-gate-service restart
```
