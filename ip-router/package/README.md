# ip-router 安装包

本目录提供两个平台原生安装包构建脚本：

- `build-macos.sh`：在 macOS 上编译并生成 `.pkg`。
- `build-ubuntu.sh`：在 Ubuntu 上编译并生成 `.deb`。

脚本只能在对应的目标平台运行，不进行交叉编译。安装包固定安装至
`/opt/soft/ip-router`，并将服务配置注册到
`/opt/soft/acl-master/conf/services.cf`。安装完成后会通过 `master_ctl`
启动或重启 ip-router。

## 前置条件

运行安装包前，acl-master 必须已经安装在：

```text
/opt/soft/acl-master
```

构建机器还需要具备本项目原有的 ACL 开发库及编译环境。macOS 需要 Xcode
Command Line Tools；Ubuntu 需要 `g++`、`make`、`dpkg-dev` 和 `file`。

## macOS

```shell
cd ip-router/package
./build-macos.sh
sudo installer -pkg ./dist/ip-router-1.0.0-macos-$(uname -m).pkg -target /
```

可将版本号和输出目录作为参数传入：

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
cd ip-router/package
./build-ubuntu.sh
sudo apt install ./dist/ip-router_1.0.0_$(dpkg --print-architecture).deb
```

同样可以指定版本和输出目录：

```shell
./build-ubuntu.sh 1.2.0 /tmp/packages
```

## 构建参数

- `IP_ROUTER_VERSION`：没有位置参数时覆盖 `VERSION` 文件中的版本。
- `BUILD_JOBS`：并行编译任务数。
- `SKIP_BUILD=1`：跳过编译，直接打包当前平台已有的 `ip-router` 二进制。
- `PACKAGE_MAINTAINER`：Ubuntu 软件包维护者字段。
- `PKG_SIGN_IDENTITY`：macOS Installer 证书名称。

## 安装后的目录

```text
/opt/soft/ip-router/
├── bin/ip-router-service
├── sbin/ip-router
├── conf/ip-router.cf
├── conf/ip-router.cf.default
├── var/html/index.html
├── var/html/tlds-alpha-by-domain.txt
├── var/log/
└── var/pid/
```

升级时不会覆盖现有的 `conf/ip-router.cf`。路由数据库、全局路由设置和日志等
运行数据也不会被安装包覆盖。

可以手工控制服务：

```shell
sudo /opt/soft/ip-router/bin/ip-router-service start
sudo /opt/soft/ip-router/bin/ip-router-service stop
sudo /opt/soft/ip-router/bin/ip-router-service restart
```
