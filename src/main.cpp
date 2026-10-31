// smalltv-mod — personal dashboard firmware for the GeekMagic SmallTV family.
//
// One persistent draw-list dashboard, one transient webhook overlay, and a
// normalized usage feed pushed by a local daemon. See src/features/view and
// src/features/usage.
#include <Arduino.h>
#include "Platform.h"
#include "config.h"
#include "Settings.h"
#include "Net.h"
#include "Gfx.h"
#include "WebPortal.h"
#include "OtaUpdate.h"
#include "Clock.h"
#include "features/view/ViewMode.h"
#include "features/view/ViewOverlay.h"
#include "features/view/ViewPoll.h"
#include "features/usage/UsageClient.h"

static Settings g_settings;
static String   g_resetReason;
static bool     g_safeMode = false;
static char     g_epcStr[16] = "";
static char     g_addrStr[16] = "";
static int      g_lastBr = -1;
static uint32_t g_btnWakeUntil = 0;
#if HAS_LDR
static uint32_t g_lastAutoBr = 0;
static uint8_t  g_ldrCache   = DEFAULT_BRIGHTNESS;
#endif

// Single brightness resolver: night mode overrides auto-brightness overrides
// the manual level. Only writes the PWM when the effective target changes.
static uint8_t appEffectiveBrightness() {
  if (g_btnWakeUntil && millis() < g_btnWakeUntil) return g_settings.brightness;
  if (clockNightActive()) return g_settings.clock.nightLevel;
#if HAS_LDR
  if (g_settings.autoBrightness) {
    if (millis() - g_lastAutoBr > 2000) {
      g_lastAutoBr = millis();
      int raw = analogRead(LDR_PIN);
      g_ldrCache = (uint8_t)constrain(raw * 100 / ADC_MAX, 5, 100);
    }
    return g_ldrCache;
  }
#endif
  return g_settings.brightness;
}

void appApplyBrightness() {
  uint8_t t = appEffectiveBrightness();
  if ((int)t != g_lastBr) {
    g_lastBr = t;
    gfxSetBrightness(t, g_settings.backlightInverted);
  }
}

#ifdef BTN_PIN
static uint32_t g_btnInfoUntil = 0;

static void buttonBlinkOff() {
  gfxSetBrightness(0, g_settings.backlightInverted);
  delay(1000);
  g_lastBr = -1;
  appApplyBrightness();
}

static void buttonShowNetInfo() {
  gfxStaInfo(netSSID().c_str(), netIP().c_str(), g_settings.hostname.c_str());
  g_btnInfoUntil = millis() + BTN_INFO_MS;
}

static void buttonWake() {
  g_btnWakeUntil = millis() + BTN_WAKE_MS;
  g_lastBr = -1;
  appApplyBrightness();
}

static void buttonGoHome() {
  g_viewOverlay.dismiss();
  g_viewMode.repaint();
}

static void buttonNext() {
  if (g_viewOverlay.active()) g_viewOverlay.dismiss();
  else                        g_viewMode.repaint();
}

static void buttonAckNotify() { g_viewOverlay.dismiss(); }

static void buttonAction(uint8_t act) {
  switch (act) {
    case BTN_ACT_SCREEN_OFF: buttonBlinkOff();    break;
    case BTN_ACT_HOME:       buttonGoHome();      break;
    case BTN_ACT_NEXT:       buttonNext();        break;
    case BTN_ACT_NETINFO:    buttonShowNetInfo(); break;
    case BTN_ACT_WAKE:       buttonWake();        break;
    case BTN_ACT_ACK:        buttonAckNotify();   break;
    default: break;
  }
}

static void buttonService() {
  static bool     inited   = false;
  static uint16_t thrDown  = 0;
  static uint16_t thrUp    = 0;
  static uint8_t  sensSeen = 0;
  static bool     stable   = false;
  static uint8_t  run      = 0;
  static uint32_t downAt   = 0;

  if (!inited || sensSeen != g_settings.button.sensitivity) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += touchRead(BTN_PIN); delay(2); }
    uint16_t baseline = sum / 16;
    uint8_t s = g_settings.button.sensitivity;
    thrDown = (uint16_t)((uint32_t)baseline * s / 100);
    uint8_t up = s + 5; if (up > 99) up = 99;
    thrUp = (uint16_t)((uint32_t)baseline * up / 100);
    sensSeen = s;
    inited = true;
  }

  uint16_t a = touchRead(BTN_PIN);
  uint16_t b = touchRead(BTN_PIN);
  uint16_t c = touchRead(BTN_PIN);
  uint16_t m = a > b ? (b > c ? b : (a > c ? c : a))
                     : (a > c ? a : (b > c ? c : b));

  bool wantDown = stable ? (m < thrUp) : (m < thrDown);
  if (wantDown == stable) { run = 0; return; }
  if (++run < BTN_CONFIRM) return;
  run = 0;
  stable = wantDown;
  if (stable) { downAt = millis(); return; }
  uint32_t held = millis() - downAt;
  buttonAction(held >= BTN_LONG_MS ? g_settings.button.longAction
                                   : g_settings.button.shortAction);
}
#endif

const char* appResetReason() { return g_resetReason.c_str(); }

void appInvalidate() {
  g_viewMode.invalidate();
  viewPollForce();
}

void appRelease() {
  g_viewMode.freeForRequest();
}

static void bootProgress(const char* msg) {
  gfxBoot("SmallTV", msg);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(FW_NAME " " FW_VERSION);

  PlatformReset pr = platformResetInfo();
  Serial.print("[boot] reset reason: ");
  Serial.println(pr.reason);

  if (pr.wasCrash) {
    g_safeMode = true;
    strlcpy(g_epcStr,  pr.epc,  sizeof(g_epcStr));
    strlcpy(g_addrStr, pr.addr, sizeof(g_addrStr));
    char rich[80];
    snprintf(rich, sizeof(rich), "%s epc %s addr %s", pr.reason.c_str(),
             g_epcStr[0] ? g_epcStr : "-", g_addrStr[0] ? g_addrStr : "-");
    g_resetReason = rich;
  } else {
    g_resetReason = pr.reason;
  }

  Serial.println("[boot] settings");
  settingsBegin();
  loadSettings(g_settings);

  Serial.println("[boot] display");
  gfxBegin(g_settings);
  gfxBoot(g_safeMode ? "Crashed" : "SmallTV", FW_VERSION);

  Serial.println("[boot] net");
  netAllowRetry(!g_safeMode);
  netBegin(g_settings, bootProgress);
  if (!g_safeMode) clockReapply(g_settings);

  if (otaBootRequested()) {
    Serial.println("[boot] github update");
    gfxBoot("SmallTV", "updating...");
    otaBootUpdate(g_settings);
    gfxBoot("SmallTV", "update failed");
    delay(1200);
  }

  Serial.println("[boot] usage");
  usageInit();

  Serial.println("[boot] web");
  webPortalBegin(g_settings);

  Serial.println("[boot] view");
  g_viewMode.begin();
  viewPollBegin(g_settings);

  Serial.println("[boot] done");

  if (netMode() == NET_AP) {
    gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str(), netFailReason());
  } else if (g_safeMode) {
    gfxCrash(g_epcStr, g_addrStr, netIP().c_str());
  } else {
    gfxStaInfo(netSSID().c_str(), netIP().c_str(), g_settings.hostname.c_str());
    delay(3500);
  }
}

void loop() {
  netLoop();
  webPortalLoop();

#ifdef BTN_PIN
  buttonService();
  if (g_btnInfoUntil && millis() >= g_btnInfoUntil) {
    g_btnInfoUntil = 0;
    if (netMode() == NET_AP) {
      gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str(), netFailReason());
    } else {
      g_viewMode.repaint();
    }
  }
#endif

  if (webPortalRebootDue()) {
    delay(120);
    ESP.restart();
  }

  if (g_safeMode) {
    delay(5);
    return;
  }

  if (netMode() == NET_AP) {
    static uint16_t shownFail = netFailSeq();
    if (netFailSeq() != shownFail) {
      shownFail = netFailSeq();
      gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str(), netFailReason());
    }
    delay(5);
    return;
  }

#ifdef BTN_PIN
  if (g_btnInfoUntil) {
    delay(5);
    return;
  }
#endif

  clockService(g_settings);
  appApplyBrightness();

  viewPollService(g_settings);
  g_viewOverlay.service();
  if (g_viewOverlay.active()) {
    delay(5);
    return;
  }

  g_viewMode.service();
  delay(5);
}
