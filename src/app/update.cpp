// UPDATING as the app sees it (FirmwareSpec.md §16, §12.3): the quiesce
// without the AP, the progress screen from the job state, the return to
// ACTIVE when a job fails, and the health check that marks a new image valid
// or rolls it back.
#include "app/state.h"
#include "diag/log.h"
#include "util/timer.h"
#include "util/strutil.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char* TAG = "update";

void AppState::enterUpdating(const char* what) {
  if (mode_ == AppMode::Updating) return;
  if (mode_ == AppMode::Menu) closeMenu(true, "update");
  uint32_t now = millis();
  LOG_W(TAG, "UPDATING (%s): audio down, pads and buttons ignored, keyboard suspended", what);
  haptics_.stop(); jacks_.allOff();                            // §12.3 quiesce, the AP kept (§16 "During install")
  press_.cancel();
  kbd_.releaseAll("update", now); kbd_.setSuspended(true);
  if (kcx_.pair() != KcxLink::Pair::None) kcx_.cancelPair();
  if (audio_.playing()) { audio_.stop(); uint32_t t0 = millis(); while (audio_.playing() && millis() - t0 < 300) { esp_task_wdt_reset(); delay(5); } }
  cache_.joinLoader();
  audio_.railDown();                                           // rule 5 order inside the engine; a flash write may follow at once (rule 17)
  if (volume_.dirty()) volumeWriteThunk(this);
  mode_ = AppMode::Updating;
  updateView_ = UpdateView();
  snprintf(updateView_.line, sizeof updateView_.line, "%s", what);
  updateScreen_.view = &updateView_;
  display_.setBrightness(cfg_.display.brightnessPct);
  display_.setScreen(&updateScreen_);
  updateTickAt_ = now;
  lastInputMs_ = now;
}

void AppState::leaveUpdating(const char* why) {
  if (mode_ != AppMode::Updating) return;
  uint32_t now = millis();
  updateView_.failed = true;
  snprintf(updateView_.line, sizeof updateView_.line, "%s", why ? why : "failed");
  display_.dirty(R_ALL);
  LOG_W(TAG, "leaving UPDATING: %s", why ? why : "");
  audio_.railUp();
  if (audioStarted_) cache_.reload(cfg_, level_);
  kbd_.setSuspended(false);
  mode_ = AppMode::Active;
  showHome();
  message(why ? why : "update failed", 6000);
  lastInputMs_ = now; levels_.onInput(now);
  refreshView(R_ALL);
}

void AppState::tickUpdating(uint32_t now) {
  if (!due(now, updateTickAt_)) return;
  updateTickAt_ = now + 250;
  const char* t = updater_.text();
  uint8_t pct = updater_.pct();
  bool failed = updater_.job() == Updater::Job::Failed;
  if (strcmp(t, updateView_.line) || pct != updateView_.pct || failed != updateView_.failed) {
    sb::copyStr(updateView_.line, sizeof updateView_.line, t);
    updateView_.pct = pct; updateView_.failed = failed;
    display_.dirty(R_ALL);
  }
}

// §16 "Health check": the image marks itself valid only after the screen is up, the configuration is loaded,
// the pads are calibrated, the audio rail is ready and 30 s have passed; if those do not all hold 90 s after
// boot it rolls back. A crash or watchdog reset while pending boots the previous slot by itself.
void AppState::healthCheck(uint32_t now) {
  if (otaMarked_) return;
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) { otaMarked_ = true; return; }
  bool healthy = display_.ready() && configLoaded_ && touch_.ready() && railReady_ && (mode_ == AppMode::Active || mode_ == AppMode::Dimmed || mode_ == AppMode::Menu);
  if (healthy && now >= 30000) {
    otaMarked_ = true;
    esp_ota_mark_app_valid_cancel_rollback();
    LOG_I(TAG, "image %s marked valid: screen, configuration, pads and audio up, 30 s of uptime", FW_VERSION);
    return;
  }
  if (now >= 90000) {
    LOG_E(TAG, "health check failed after 90 s (screen %d, config %d, pads %d, audio %d, mode %s): rolling back",
          display_.ready(), configLoaded_, touch_.ready(), railReady_, modeName());
    Log::tick(); Serial.flush(); delay(50);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}
