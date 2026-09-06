#pragma once

// 在独立协程调度线程中启动 dns-gate 的只读 HTTP 诊断服务。
void dns_http_service_start(const char* address);
