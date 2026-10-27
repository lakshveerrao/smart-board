// board_logic.h - Smart Board game-controller brain (plain C++, no Arduino needed)
//
// Feed it accelerometer readings; it gives back which keys (W/A/S/D) to hold
// and tells you about ground touches, warnings and lock-outs.
//
// Works however the sensor is mounted: calibration learns "level" and
// "forward" from you, so nothing about axes is hard-coded.
#pragma once
#include <math.h>
#include <stdint.h>

namespace sb {

// ---------------- tuning (all angles in degrees, times in ms) ----------------
struct Config {
  float keyOnDeg        = 4.0f;    // lean this far to press a key
  float keyOffDeg       = 3.0f;    // ...and come back under this to release (no flicker)
  float defaultGroundDeg= 19.5f;   // where the board edge meets the floor (your data: 19-20.5)
  float touchMarginDeg  = 3.0f;    // touch = within this many degrees of the ground angle
  float touchMinDeg     = 15.0f;   // never call a touch below this
  float groundLearnDown = 1.5f;    // learned ground angle may only move this far below default...
  float groundLearnUp   = 2.5f;    // ...or this far above it (stops sensitivity drifting)
  uint32_t touchHoldMs  = 200;     // must stay down this long to count as a touch
  float releaseExtraDeg = 2.0f;    // must come this far back up to end the touch
  uint32_t releaseHoldMs= 150;
  uint8_t  maxWarnings  = 3;       // 3rd touch = lock-out
  float levelDeg        = 7.0f;    // "roughly centred" for unlock
  uint32_t levelHoldMs  = 600;     // ...held this long
  uint32_t maxLockMs    = 5000;    // lock never lasts longer than this
  uint32_t warnForgetMs = 20000;   // 20 s with no touch wipes your warnings
  float calStillDeg     = 2.0f;    // calibration: "still" means moving less than this
  uint32_t calLevelMs   = 1500;    // hold level this long to set neutral
  float calLeanDeg      = 10.0f;   // calibration: lean forward at least this far
  uint32_t calLeanMs    = 600;
  float filterAlpha     = 0.25f;   // smoothing of accel (0..1, lower = smoother)
};

enum Key : uint8_t { KEY_W = 1, KEY_A = 2, KEY_S = 4, KEY_D = 8 };

enum Mode : uint8_t { CAL_LEVEL, CAL_FORWARD, PLAY, TOUCHING, LOCKED };

enum Event : uint8_t {
  EV_NONE = 0,
  EV_LEVEL_SET,      // calibration step 1 done
  EV_CALIBRATED,     // calibration step 2 done - playing
  EV_TOUCH,          // ground touch (warning number in warnings())
  EV_TOUCH_END,
  EV_LOCKED,         // too many touches - keys off until level
  EV_UNLOCKED,
  EV_WARNINGS_CLEARED
};

struct V3 { float x, y, z; };
static inline V3 v(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline float dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3 sub(V3 a, V3 b) { return v(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline V3 scl(V3 a, float s) { return v(a.x*s, a.y*s, a.z*s); }
static inline V3 cross(V3 a, V3 b) { return v(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static inline float len(V3 a) { return sqrtf(dot(a, a)); }
static inline V3 norm(V3 a) { float l = len(a); return l > 1e-6f ? scl(a, 1.0f/l) : v(0,0,1); }
static inline float deg(float r) { return r * 57.2957795f; }
static inline float angleBetween(V3 a, V3 b) {           // a, b unit
  float c = dot(a, b); if (c > 1) c = 1; if (c < -1) c = -1; return deg(acosf(c));
}

class BoardLogic {
 public:
  Config cfg;

  void begin() { mode_ = CAL_LEVEL; started_ = false; keys_ = 0; warnings_ = 0; }

  // Skip calibration with stored values (unit vectors).
  void setCalibration(V3 up, V3 fwdSig) {
    up_ = norm(up); setForward(fwdSig); mode_ = PLAY; resetGround();
  }
  bool calibrated() const { return mode_ >= PLAY; }
  V3 up() const { return up_; }
  V3 fwdSig() const { return fwd_; }

  // Call every sample. ax/ay/az in any unit (raw counts fine). Returns an event.
  Event update(float ax, float ay, float az, uint32_t now) {
    V3 raw = norm(v(ax, ay, az));
    if (!started_) { g_ = raw; started_ = true; stillSince_ = now; ref_ = raw; }
    g_ = norm(v(g_.x + cfg.filterAlpha*(raw.x-g_.x), g_.y + cfg.filterAlpha*(raw.y-g_.y),
                g_.z + cfg.filterAlpha*(raw.z-g_.z)));

    switch (mode_) {
      case CAL_LEVEL:   return calLevel(now);
      case CAL_FORWARD: return calForward(now);
      default:          return play(now);
    }
  }

  uint8_t keys() const { return keys_; }
  Mode mode() const { return mode_; }
  uint8_t warnings() const { return warnings_; }
  float tiltDeg() const { return tilt_; }
  float fwdDeg() const { return fwdDeg_; }      // + forward / - back
  float rightDeg() const { return rightDeg_; }  // + right   / - left
  float groundDeg(int sector) const { return ground_[sector & 7]; }
  int sector() const { return sector_; }

  void recalibrate() { begin(); }

 private:
  Mode mode_ = CAL_LEVEL;
  bool started_ = false;
  V3 g_ = {0,0,1}, up_ = {0,0,1}, fwd_ = {0,1,0}, right_ = {1,0,0}, ref_ = {0,0,1};
  uint32_t stillSince_ = 0, leanSince_ = 0, touchSince_ = 0, releaseSince_ = 0,
           levelSince_ = 0, lastTouch_ = 0, lockSince_ = 0;
  bool touchPending_ = false, releasePending_ = false, levelPending_ = false;
  uint8_t keys_ = 0, warnings_ = 0;
  float tilt_ = 0, fwdDeg_ = 0, rightDeg_ = 0;
  float ground_[8];
  int sector_ = 0;

  void resetGround() { for (int i = 0; i < 8; i++) ground_[i] = cfg.defaultGroundDeg; }

  void setForward(V3 sig) {
    // "sig" = how gravity shifts when you lean forward. Project it flat.
    V3 p = sub(sig, scl(up_, dot(sig, up_)));
    fwd_ = norm(p);
    right_ = cross(fwd_, up_);   // leaning right shifts gravity this way (right-handed sensor)
  }

  // Step 1: stand level and still
  Event calLevel(uint32_t now) {
    if (angleBetween(g_, ref_) > cfg.calStillDeg) { ref_ = g_; stillSince_ = now; return EV_NONE; }
    if (now - stillSince_ >= cfg.calLevelMs) {
      up_ = g_; mode_ = CAL_FORWARD; leanSince_ = 0; return EV_LEVEL_SET;
    }
    return EV_NONE;
  }

  // Step 2: lean forward and hold
  Event calForward(uint32_t now) {
    float t = angleBetween(g_, up_);
    if (t < cfg.calLeanDeg) { leanSince_ = 0; return EV_NONE; }
    if (!leanSince_) { leanSince_ = now; ref_ = g_; return EV_NONE; }
    if (angleBetween(g_, ref_) > 4.0f) { leanSince_ = now; ref_ = g_; return EV_NONE; }  // wandering
    if (now - leanSince_ >= cfg.calLeanMs) {
      setForward(g_); resetGround(); mode_ = PLAY; warnings_ = 0; return EV_CALIBRATED;
    }
    return EV_NONE;
  }

  float touchThreshold(int s) const {
    float th = ground_[s] - cfg.touchMarginDeg;
    return th < cfg.touchMinDeg ? cfg.touchMinDeg : th;
  }

  Event play(uint32_t now) {
    // where are we leaning?
    tilt_ = angleBetween(g_, up_);
    V3 p = sub(g_, scl(up_, dot(g_, up_)));
    fwdDeg_ = deg(asinf(fmaxf(-1.f, fminf(1.f, dot(p, fwd_)))));
    rightDeg_ = deg(asinf(fmaxf(-1.f, fminf(1.f, dot(p, right_)))));
    float dir = atan2f(rightDeg_, fwdDeg_);                       // 0 = forward, +90 = right
    int s = (int)lroundf(dir / 0.785398f); s = ((s % 8) + 8) % 8;   // 8 sectors
    if (tilt_ > 6.0f) sector_ = s;

    // forget old warnings
    if (warnings_ && mode_ == PLAY && now - lastTouch_ > cfg.warnForgetMs) {
      warnings_ = 0; return EV_WARNINGS_CLEARED;
    }

    if (mode_ == LOCKED) {
      keys_ = 0;
      bool centred = false;
      if (tilt_ < cfg.levelDeg) {
        if (!levelPending_) { levelPending_ = true; levelSince_ = now; }
        else if (now - levelSince_ >= cfg.levelHoldMs) centred = true;
      } else levelPending_ = false;
      if (centred || now - lockSince_ >= cfg.maxLockMs) {
        levelPending_ = false; warnings_ = 0; lastTouch_ = now; mode_ = PLAY; return EV_UNLOCKED;
      }
      return EV_NONE;
    }

    if (mode_ == TOUCHING) {
      keys_ = 0;
      // learn this board's real ground angle in this direction - only from
      // readings near the expected ground angle, and only within safe limits
      float lo = cfg.defaultGroundDeg - cfg.groundLearnDown, hi = cfg.defaultGroundDeg + cfg.groundLearnUp;
      if (tilt_ >= lo - 1.0f && tilt_ <= hi + 1.0f) {
        float gnew = ground_[sector_] + 0.05f * (tilt_ - ground_[sector_]);
        ground_[sector_] = gnew < lo ? lo : (gnew > hi ? hi : gnew);
      }
      if (tilt_ < touchThreshold(sector_) - cfg.releaseExtraDeg) {
        if (!releasePending_) { releasePending_ = true; releaseSince_ = now; }
        else if (now - releaseSince_ >= cfg.releaseHoldMs) {
          releasePending_ = false; lastTouch_ = now;
          if (warnings_ >= cfg.maxWarnings) { mode_ = LOCKED; levelPending_ = false; lockSince_ = now; return EV_LOCKED; }
          mode_ = PLAY; return EV_TOUCH_END;
        }
      } else releasePending_ = false;
      return EV_NONE;
    }

    // PLAY: touch check
    if (tilt_ >= touchThreshold(sector_)) {
      if (!touchPending_) { touchPending_ = true; touchSince_ = now; }
      else if (now - touchSince_ >= cfg.touchHoldMs) {
        touchPending_ = false; releasePending_ = false;
        mode_ = TOUCHING; keys_ = 0; lastTouch_ = now;
        if (warnings_ < 255) warnings_++;
        return EV_TOUCH;
      }
    } else touchPending_ = false;

    // keys, with hysteresis
    uint8_t k = keys_;
    k = hyst(k, KEY_W, fwdDeg_);
    k = hyst(k, KEY_S, -fwdDeg_);
    k = hyst(k, KEY_D, rightDeg_);
    k = hyst(k, KEY_A, -rightDeg_);
    keys_ = k;
    return EV_NONE;
  }

  uint8_t hyst(uint8_t k, uint8_t bit, float deg) const {
    if (deg >= cfg.keyOnDeg) return k | bit;
    if (deg < cfg.keyOffDeg) return k & ~bit;
    return k;
  }
};

}  // namespace sb
