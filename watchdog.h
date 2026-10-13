// homelab-watchdog checker: runs in its own FreeRTOS task so slow/failed connects never block the ESPHome loop.
// Each target is checked every ~60 s; 3 failures in a row = DOWN alert, first success after that = RECOVERED alert.
// Alerts go straight to a Discord webhook (queued and retried if the internet is down); Kuma gets a push each cycle.
#pragma once
#include <string>
#include <deque>
#include <atomic>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"

namespace wd {

static const char *TAG = "watchdog";
enum Kind { TCP, DNS };
struct Target { const char *name; const char *host; uint16_t port; Kind kind; };

// Verified 2026-10-13 from the G4 (TCP connect / DNS answer).
static const Target TARGETS[] = {
  {"Uptime Kuma (Zima :3001)",   "192.168.8.149", 3001, TCP},
  {"ZimaBlade (SSH :22)",         "192.168.8.149", 22,   TCP},
  {"NPMplus (Zima :80)",          "192.168.8.149", 80,   TCP},
  {"C1-G4 Proxmox (:8006)",       "192.168.8.20",  8006, TCP},
  {"c1-pn2 Proxmox (:8006)",      "192.168.8.10",  8006, TCP},
  {"Hermes CT100 (Caddy :443)",   "192.168.8.120", 443,  TCP},
  {"Vaultwarden CT110 (:8000)",   "192.168.8.30",  8000, TCP},
  {"Flint router (:80)",          "192.168.8.1",   80,   TCP},
  {"AdGuard DNS (Flint :3053)",   "192.168.8.1",   3053, DNS},
  {"Internet (1.1.1.1 :443)",     "1.1.1.1",       443,  TCP},
};
static const int N = sizeof(TARGETS) / sizeof(TARGETS[0]);
static const int STRIKES = 3;
static const int CYCLE_S = 60;

struct State { std::atomic<int> fails{0}; std::atomic<bool> down{false}; std::atomic<bool> up{false};
               std::atomic<bool> checked{false}; int64_t down_since = 0; };
static State STATE[N];
static std::atomic<int> DOWN_COUNT{0};
static std::atomic<int> QUEUED{0};
static std::atomic<uint32_t> CYCLES{0};
static std::string WEBHOOK, KUMA_PUSH;
static std::deque<std::string> QUEUE;
static SemaphoreHandle_t QMUX = nullptr;
static std::atomic<bool> NET_UP{false};

static bool tcp_check(const char *host, uint16_t port, int timeout_ms) {
  struct sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return false;
  int s = ::socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return false;
  ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  int r = ::connect(s, (struct sockaddr *) &a, sizeof(a));
  bool ok = (r == 0);
  if (!ok && errno == EINPROGRESS) {
    fd_set w; FD_ZERO(&w); FD_SET(s, &w);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(s + 1, nullptr, &w, nullptr, &tv) > 0) {
      int err = 0; socklen_t l = sizeof(err);
      ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l); ok = (err == 0);
    }
  }
  ::close(s); return ok;
}

// A real A-record query for cloudflare.com straight at AdGuard (not via dnsmasq's cache).
static bool dns_check(const char *host, uint16_t port, int timeout_ms) {
  static const uint8_t q[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              10, 'c','l','o','u','d','f','l','a','r','e', 3, 'c','o','m', 0, 0x00, 0x01, 0x00, 0x01};
  struct sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return false;
  int s = ::socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return false;
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  bool ok = false; uint8_t buf[512];
  if (::sendto(s, q, sizeof(q), 0, (struct sockaddr *) &a, sizeof(a)) == (int) sizeof(q)) {
    int n = ::recv(s, buf, sizeof(buf), 0);
    ok = n >= 12 && buf[0] == 0x12 && buf[1] == 0x34 && (buf[3] & 0x0F) == 0 && ((buf[6] << 8) | buf[7]) > 0;
  }
  ::close(s); return ok;
}

static int http_req(const std::string &url, const char *body) {
  if (url.empty()) return -1;
  esp_http_client_config_t c = {};
  c.url = url.c_str(); c.timeout_ms = 8000; c.crt_bundle_attach = esp_crt_bundle_attach;
  c.method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  esp_http_client_handle_t h = esp_http_client_init(&c); if (!h) return -1;
  if (body) { esp_http_client_set_header(h, "Content-Type", "application/json");
              esp_http_client_set_post_field(h, body, strlen(body)); }
  int code = (esp_http_client_perform(h) == ESP_OK) ? esp_http_client_get_status_code(h) : -1;
  esp_http_client_cleanup(h); return code;
}

static void enqueue(const std::string &msg) {
  xSemaphoreTake(QMUX, portMAX_DELAY);
  if (QUEUE.size() >= 20) QUEUE.pop_front();   // internet down for ages: keep the newest 20
  QUEUE.push_back(msg); QUEUED = QUEUE.size();
  xSemaphoreGive(QMUX);
}

static void flush_queue() {
  while (true) {
    xSemaphoreTake(QMUX, portMAX_DELAY);
    if (QUEUE.empty()) { xSemaphoreGive(QMUX); return; }
    std::string m = QUEUE.front(); xSemaphoreGive(QMUX);
    std::string body = "{\"username\":\"homelab-watchdog\",\"content\":\"" + m + "\"}";
    int code = http_req(WEBHOOK, body.c_str());
    if (code < 200 || code >= 300) { ESP_LOGW(TAG, "discord send failed (%d), will retry", code); return; }
    xSemaphoreTake(QMUX, portMAX_DELAY); QUEUE.pop_front(); QUEUED = QUEUE.size(); xSemaphoreGive(QMUX);
    vTaskDelay(pdMS_TO_TICKS(1200));            // stay well under Discord's webhook rate limit
  }
}

static void task(void *) {
  bool hello = false;
  while (true) {
    int64_t start = esp_timer_get_time();
    if (NET_UP) {
      if (!hello) { enqueue("🛡️ watchdog online: checking " + std::to_string(N) + " targets every 60 s"); hello = true; }
      int down = 0;
      for (int i = 0; i < N; i++) {
        const Target &t = TARGETS[i]; State &s = STATE[i];
        bool ok = t.kind == DNS ? dns_check(t.host, t.port, 2000) : tcp_check(t.host, t.port, 3000);
        s.up = ok; s.checked = true;
        if (ok) {
          if (s.down) {
            int mins = (int) ((esp_timer_get_time() - s.down_since) / 60000000LL);
            enqueue(std::string("🟢 RECOVERED: ") + t.name + " (down ~" + std::to_string(mins) + " min)");
          }
          s.fails = 0; s.down = false;
        } else {
          int f = ++s.fails;
          if (f == STRIKES && !s.down) {
            s.down = true; s.down_since = esp_timer_get_time();
            enqueue(std::string("🔴 DOWN: ") + t.name + " (" + t.host + ":" + std::to_string(t.port) +
                    ") failed " + std::to_string(STRIKES) + " checks in a row");
          }
        }
        if (s.down) down++;
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      DOWN_COUNT = down; CYCLES++;
      if (!KUMA_PUSH.empty()) {   // mutual watch: Kuma alerts if these pushes stop
        std::string u = KUMA_PUSH + (KUMA_PUSH.find('?') == std::string::npos ? "?" : "&") +
                        "status=up&msg=" + std::to_string(N - down) + "of" + std::to_string(N) + "up";
        int code = http_req(u, nullptr);
        if (code != 200) ESP_LOGW(TAG, "kuma push failed (%d)", code);
      }
      flush_queue();
    }
    int64_t spent_ms = (esp_timer_get_time() - start) / 1000;
    int wait = CYCLE_S * 1000 - (int) spent_ms;
    vTaskDelay(pdMS_TO_TICKS(wait > 1000 ? wait : 1000));
  }
}

static void start(const std::string &webhook, const std::string &kuma) {
  if (QMUX) return;
  WEBHOOK = webhook; KUMA_PUSH = kuma; QMUX = xSemaphoreCreateMutex();
  xTaskCreate(task, "watchdog", 8192, nullptr, 5, nullptr);
}

static std::string status(int i) {
  const State &s = STATE[i];
  if (!s.checked) return "waiting";
  if (s.down) return "DOWN";
  if (!s.up) return "failing (" + std::to_string(s.fails.load()) + "/" + std::to_string(STRIKES) + ")";
  return "up";
}

}  // namespace wd
