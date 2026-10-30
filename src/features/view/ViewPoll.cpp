#include "ViewPoll.h"
#include "Platform.h"
#include "Net.h"
#include "ViewOverlay.h"
#include <memory>

static uint32_t g_nextMs = 0;
static String   g_error;

void viewPollBegin(const Settings& s) {
  (void)s;
  g_nextMs = 0;
  g_error = "";
}

void viewPollForce() { g_nextMs = 0; }

const String& viewPollError() { return g_error; }

static void fetchOverlay(const Settings& s) {
  const String& url = s.view.pollUrl;
  const bool https = url.startsWith("https://");

  std::unique_ptr<NetClient> client;
  if (https) {
    if (platformMaxFreeBlock() < 16000) { g_error = F("low heap"); return; }
    client.reset(platformMakeSecureClient(4096));
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(*client, url)) { g_error = F("connect failed"); return; }
  http.addHeader("Accept", "application/json");
  http.setUserAgent(F(FW_NAME));
  int code = http.GET();
  if (code == 204) {
    g_viewOverlay.dismiss();
    g_error = "";
    http.end();
    return;
  }
  if (code != HTTP_CODE_OK) {
    g_error = "HTTP " + String(code);
    http.end();
    return;
  }
  const int declared = http.getSize();
  if (declared > (int)VIEW_MAX_JSON) {
    g_error = F("view too large");
    http.end();
    return;
  }
  String body;
  if (declared > 0) body.reserve((unsigned)declared);
  Stream& stream = http.getStream();
  const uint32_t started = millis();
  while (body.length() < VIEW_MAX_JSON) {
    int available = stream.available();
    if (available > 0) {
      char buffer[64];
      size_t count = (size_t)available < sizeof(buffer) ? (size_t)available : sizeof(buffer);
      int read = stream.readBytes(buffer, count);
      if (read <= 0) break;
      body.concat(buffer, (unsigned)read);
    } else if (!client->connected()) {
      if (stream.available() == 0) break;
    } else if ((uint32_t)(millis() - started) > s.httpTimeout) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();

  if (body.length() == 0) {
    g_viewOverlay.dismiss();
    g_error = "";
    return;
  }
  String error;
  if (g_viewOverlay.show(body, s.view.ttlSec, error)) g_error = "";
  else g_error = error;
}

void viewPollService(const Settings& s) {
  if (s.view.pollSec == 0 || s.view.pollUrl.length() < 8) return;
  if (!netConnected()) return;
  if ((int32_t)(millis() - g_nextMs) < 0) return;
  g_nextMs = millis() + (uint32_t)s.view.pollSec * 1000UL;
  fetchOverlay(s);
}
