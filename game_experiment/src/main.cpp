#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

namespace {

constexpr uint8_t kBuzzerPin = 15;
constexpr uint8_t kIrRecvPin = 10;

constexpr uint8_t kTftSckPin = 21;
constexpr uint8_t kTftMosiPin = 47;
constexpr uint8_t kTftMisoPin = 48;
constexpr uint8_t kTftCsPin = 42;
constexpr uint8_t kTftDcPin = 41;
constexpr uint8_t kTftBacklightPin = 14;

constexpr uint8_t kTouchCsPin = 46;
constexpr uint8_t kTouchIrqPin = 2;
constexpr uint16_t kTouchMinX = 240;
constexpr uint16_t kTouchMaxX = 3880;
constexpr uint16_t kTouchMinY = 240;
constexpr uint16_t kTouchMaxY = 3880;

constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;
constexpr uint8_t kScreenRotation = 1;

constexpr int kGameWidth = 250;
constexpr int kControlX = 250;
constexpr int kControlWidth = 70;
constexpr int kButtonHeight = 80;

constexpr int kPlayerX = 10;
constexpr int kPlayerWidth = 18;
constexpr int kPlayerHeight = 18;
constexpr int kEnemyX = 210;

constexpr uint16_t kColorBg = ST77XX_BLACK;
constexpr uint16_t kColorHud = ST77XX_WHITE;
constexpr uint16_t kColorAccent = ST77XX_CYAN;
constexpr uint16_t kColorEnemy = ST77XX_RED;
constexpr uint16_t kColorEnemyCore = ST77XX_BLACK;
constexpr uint16_t kColorShip = ST77XX_YELLOW;
constexpr uint16_t kColorPanel = ST77XX_BLUE;
constexpr uint16_t kColorFire = ST77XX_ORANGE;
constexpr uint16_t kColorPressed = ST77XX_GREEN;

constexpr uint16_t kFrameTimeMs = 30;
constexpr uint16_t kIrRepeatWindowMs = 220;

constexpr int kShipBitmapWidth = 16;
constexpr int kShipBitmapHeight = 16;
constexpr uint8_t kShipBitmap[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x3F, 0xF0, 0x3C, 0x00, 0x3C, 0x00,
    0xFF, 0x00, 0x7F, 0xFF, 0x7F, 0xFF, 0xFF, 0x00, 0x3C, 0x00, 0x3C, 0x00,
    0x1F, 0xF0, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00};

constexpr int kStars[][2] = {
    {12, 10},   {34, 48},   {78, 30},   {98, 70},   {124, 28},
    {148, 86},  {178, 18},  {198, 52},  {224, 78},  {238, 18},
    {60, 112},  {42, 176},  {92, 154},  {122, 202}, {166, 150},
    {190, 126}, {224, 190}, {14, 130},  {24, 220},  {214, 214}};

constexpr int kNoteF4 = 349;
constexpr int kNoteA4 = 440;
constexpr int kNoteC5 = 523;

struct Bullet {
  constexpr Bullet() = default;
  constexpr Bullet(int xValue, int yValue, int radiusValue, bool activeValue)
      : x(xValue), y(yValue), radius(radiusValue), active(activeValue) {}

  int x = kEnemyX;
  int y = 0;
  int radius = 3;
  bool active = false;
};

struct Controls {
  bool up = false;
  bool down = false;
  bool fire = false;
  bool touch = false;
  uint16_t x = 0;
  uint16_t y = 0;
};

enum class IrAction : uint8_t {
  None = 0,
  Up,
  Down,
  Fire,
};

struct GameState {
  int playerY = 110;
  int playerShotX = 0;
  int playerShotY = 0;
  bool playerShotActive = false;

  int enemyY = 40;
  bool enemyMovingDown = true;
  int enemyMoveStep = 3;
  int enemyRadius = 18;

  Bullet enemyShots[4] = {
      {kEnemyX, 0, 4, false},
      {kEnemyX, 0, 3, false},
      {kEnemyX, 0, 5, false},
      {kEnemyX, 0, 4, false},
  };
  int enemyShotSpeed = 7;

  int score = 0;
  int lives = 5;
  int level = 1;
  bool gameOver = false;

  unsigned long now = 0;
  unsigned long startMs = 0;
  unsigned long lastLevelUpMs = 0;
  unsigned long enemyFireStartMs = 0;
  unsigned long enemyFireDelayMs = 0;
  bool enemyFireArmed = false;
};

SPIClass gSpi(FSPI);
Adafruit_ST7789 tft(&gSpi, kTftCsPin, kTftDcPin, -1);
XPT2046_Touchscreen touch(kTouchCsPin, kTouchIrqPin);
IRrecv irrecv(kIrRecvPin);
decode_results irResults;

GameState game;
unsigned long lastFrameMs = 0;
unsigned long lastTouchPrintMs = 0;
IrAction lastIrAction = IrAction::None;
unsigned long lastIrActionMs = 0;
bool touchReady = false;

struct RenderState {
  bool initialized = false;
  bool controlsValid = false;
  bool hudValid = false;

  int playerY = 0;
  int enemyY = 0;
  int enemyRadius = 0;

  bool playerShotActive = false;
  int playerShotX = 0;
  int playerShotY = 0;

  Bullet enemyShots[4] = {};
  Controls controls = {};

  int score = 0;
  int lives = 0;
  int level = 0;
  uint32_t timeSec = 0;
};

RenderState renderState;

void beep(int note, int durationMs) {
  tone(kBuzzerPin, note, durationMs);
  delay(durationMs);
  noTone(kBuzzerPin);
  delay(40);
}

void resetGame() {
  game = GameState{};
  game.startMs = millis();
  game.lastLevelUpMs = game.startMs;
  renderState = RenderState{};
}

int clampPlayerY(int y) {
  return constrain(y, 20, static_cast<int>(kScreenHeight) - kPlayerHeight - 4);
}

void drawStars() {
  for (const auto &star : kStars) {
    tft.drawPixel(star[0], star[1], kColorAccent);
  }
}

void drawControlPanel(const Controls &controls) {
  tft.fillRect(kControlX, 0, kControlWidth, kScreenHeight, kColorPanel);
  tft.drawFastVLine(kControlX, 0, kScreenHeight, ST77XX_WHITE);

  const uint16_t upColor = controls.up ? kColorPressed : kColorPanel;
  const uint16_t fireColor = controls.fire ? kColorPressed : kColorFire;
  const uint16_t downColor = controls.down ? kColorPressed : kColorPanel;

  tft.fillRect(kControlX + 4, 4, kControlWidth - 8, kButtonHeight - 8, upColor);
  tft.fillRect(kControlX + 4, kButtonHeight + 4, kControlWidth - 8, kButtonHeight - 8, fireColor);
  tft.fillRect(kControlX + 4, (kButtonHeight * 2) + 4, kControlWidth - 8, kButtonHeight - 8, downColor);

  tft.drawRect(kControlX + 4, 4, kControlWidth - 8, kButtonHeight - 8, ST77XX_WHITE);
  tft.drawRect(kControlX + 4, kButtonHeight + 4, kControlWidth - 8, kButtonHeight - 8, ST77XX_WHITE);
  tft.drawRect(kControlX + 4, (kButtonHeight * 2) + 4, kControlWidth - 8, kButtonHeight - 8, ST77XX_WHITE);

  tft.setTextColor(ST77XX_WHITE, upColor);
  tft.setTextSize(2);
  tft.setCursor(kControlX + 14, 28);
  tft.print("UP");
  tft.setTextColor(ST77XX_WHITE, fireColor);
  tft.setCursor(kControlX + 8, 108);
  tft.print("FIRE");
  tft.setTextColor(ST77XX_WHITE, downColor);
  tft.setCursor(kControlX + 2, 188);
  tft.print("DOWN");
}

void drawHud() {
  tft.fillRect(0, 0, kGameWidth, 16, kColorBg);
  tft.setTextSize(1);
  tft.setTextColor(kColorHud, kColorBg);
  tft.setCursor(6, 6);
  tft.printf("Score:%d", game.score);
  tft.setCursor(80, 6);
  tft.printf("Lives:%d", game.lives);
  tft.setCursor(150, 6);
  tft.printf("Lv:%d", game.level);
  tft.setCursor(190, 6);
  tft.printf("Time:%lus", game.now / 1000UL);
}

bool controlsChanged(const Controls &a, const Controls &b) {
  return a.up != b.up || a.down != b.down || a.fire != b.fire;
}

void drawIntro() {
  tft.fillScreen(kColorBg);
  drawStars();
  tft.setTextColor(kColorAccent, kColorBg);
  tft.setTextSize(3);
  tft.setCursor(24, 42);
  tft.print("xWing");
  tft.setCursor(24, 78);
  tft.print("vs");
  tft.setCursor(24, 114);
  tft.print("Death");
  tft.setCursor(24, 150);
  tft.print("Star");

  tft.setTextColor(ST77XX_WHITE, kColorBg);
  tft.setTextSize(2);
  tft.setCursor(24, 198);
  tft.print("Touch/IR to start");
}

void drawGameOverScreen() {
  tft.fillScreen(kColorBg);
  drawStars();
  tft.setTextColor(ST77XX_RED, kColorBg);
  tft.setTextSize(3);
  tft.setCursor(18, 40);
  tft.print("GAME OVER");

  tft.setTextColor(ST77XX_WHITE, kColorBg);
  tft.setTextSize(2);
  tft.setCursor(24, 96);
  tft.printf("Score: %d", game.score);
  tft.setCursor(24, 126);
  tft.printf("Level: %d", game.level);
  tft.setCursor(24, 156);
  tft.printf("Time: %lus", game.now / 1000UL);

  tft.fillRoundRect(34, 192, 220, 32, 8, kColorFire);
  tft.drawRoundRect(34, 192, 220, 32, 8, ST77XX_WHITE);
  tft.setCursor(50, 201);
  tft.print("Touch/IR to retry");
}

bool readTouch(uint16_t &x, uint16_t &y) {
  if (!touchReady || !touch.touched()) {
    return false;
  }

  TS_Point point = touch.getPoint();
  point.x = constrain(point.x, kTouchMinX, kTouchMaxX);
  point.y = constrain(point.y, kTouchMinY, kTouchMaxY);

  x = static_cast<uint16_t>(constrain(
      map(point.x, kTouchMinX, kTouchMaxX, 0, static_cast<long>(kScreenWidth) - 1),
      0L, static_cast<long>(kScreenWidth) - 1));
  y = static_cast<uint16_t>(constrain(
      map(point.y, kTouchMinY, kTouchMaxY, 0, static_cast<long>(kScreenHeight) - 1),
      0L, static_cast<long>(kScreenHeight) - 1));

  const unsigned long now = millis();
  if (now - lastTouchPrintMs > 150) {
    Serial.printf("[TOUCH] raw=(%d,%d) xy=(%u,%u)\r\n", point.x, point.y, x, y);
    lastTouchPrintMs = now;
  }
  return true;
}

IrAction mapIrCommand(uint32_t cmd) {
  switch (cmd) {
    case 0x18:  // "2"
    case 0x46:  // "CH"
    case 0x40:  // "NEXT"
      return IrAction::Up;
    case 0x52:  // "8"
    case 0x15:  // "VOL+"
    case 0x19:  // "100+"
      return IrAction::Down;
    case 0x1C:  // "5"
    case 0x43:  // "PLAY/PAUSE"
    case 0x44:  // "PREV"
      return IrAction::Fire;
    default:
      return IrAction::None;
  }
}

IrAction mapIrValue(uint64_t value) {
  switch (value) {
    case 0xFF18E7:  // "2"
    case 0xFF629D:  // "CH"
    case 0xFF02FD:  // "NEXT"
      return IrAction::Up;
    case 0xFF4AB5:  // "8"
    case 0xFFA857:  // "VOL+"
    case 0xFF9867:  // "100+"
      return IrAction::Down;
    case 0xFF38C7:  // "5"
    case 0xFFC23D:  // "PLAY/PAUSE"
    case 0xFF22DD:  // "PREV"
      return IrAction::Fire;
    default:
      return IrAction::None;
  }
}

IrAction pollIrAction() {
  if (!irrecv.decode(&irResults)) {
    return IrAction::None;
  }

  IrAction action = IrAction::None;
  const unsigned long now = millis();

  if (irResults.repeat && (now - lastIrActionMs) <= kIrRepeatWindowMs) {
    action = lastIrAction;
  } else {
    action = mapIrCommand(irResults.command);
    if (action == IrAction::None) {
      action = mapIrValue(irResults.value);
    }
  }

  Serial.print(F("[IR] cmd=0x"));
  Serial.print(static_cast<uint32_t>(irResults.command), HEX);
  Serial.print(F(" value=0x"));
  serialPrintUint64(irResults.value, HEX);
  Serial.print(F(" repeat="));
  Serial.print(irResults.repeat ? 1 : 0);
  Serial.print(F(" action="));
  Serial.println(static_cast<uint8_t>(action));

  if (action != IrAction::None) {
    lastIrAction = action;
    lastIrActionMs = now;
  }

  irrecv.resume();
  return action;
}

Controls readControls() {
  Controls controls;
  controls.touch = readTouch(controls.x, controls.y);
  const IrAction irAction = pollIrAction();

  if (controls.touch) {
    if (controls.x < kGameWidth) {
      game.playerY = clampPlayerY(static_cast<int>(controls.y) - (kPlayerHeight / 2));
    } else if (controls.y < kButtonHeight) {
      controls.up = true;
    } else if (controls.y < (kButtonHeight * 2)) {
      controls.fire = true;
    } else {
      controls.down = true;
    }
  }

  if (irAction == IrAction::Up) {
    controls.up = true;
  } else if (irAction == IrAction::Down) {
    controls.down = true;
  } else if (irAction == IrAction::Fire) {
    controls.fire = true;
  }

  return controls;
}

void firePlayerShot() {
  if (game.playerShotActive) {
    return;
  }

  game.playerShotActive = true;
  game.playerShotX = kPlayerX + kPlayerWidth;
  game.playerShotY = game.playerY + (kPlayerHeight / 2);
  tone(kBuzzerPin, 1400, 20);
}

void armEnemyFire() {
  if (game.enemyFireArmed) {
    return;
  }

  game.enemyFireStartMs = game.now;
  game.enemyFireDelayMs = random(450, 1300);
  game.enemyFireArmed = true;
}

void spawnEnemyShot() {
  for (Bullet &shot : game.enemyShots) {
    if (shot.active) {
      continue;
    }
    shot.active = true;
    shot.x = kEnemyX - game.enemyRadius;
    shot.y = game.enemyY;
    return;
  }
}

void updateDifficulty() {
  if ((game.now - game.lastLevelUpMs) < 30000UL) {
    return;
  }

  game.lastLevelUpMs = game.now;
  ++game.level;
  game.enemyShotSpeed = min(game.enemyShotSpeed + 1, 14);
  game.enemyMoveStep = min(game.enemyMoveStep + 1, 7);
  if ((game.level % 2) == 0 && game.enemyRadius > 10) {
    --game.enemyRadius;
  }
}

void updateEnemy() {
  game.enemyY += game.enemyMovingDown ? game.enemyMoveStep : -game.enemyMoveStep;
  if (game.enemyY >= (kScreenHeight - game.enemyRadius - 4)) {
    game.enemyMovingDown = false;
  }
  if (game.enemyY <= (game.enemyRadius + 20)) {
    game.enemyMovingDown = true;
  }
}

void updatePlayerShot() {
  if (!game.playerShotActive) {
    return;
  }

  game.playerShotX += 18;
  if (game.playerShotX >= kGameWidth) {
    game.playerShotActive = false;
  }
}

void updateEnemyShots() {
  armEnemyFire();
  if (game.enemyFireArmed &&
      (game.now - game.enemyFireStartMs) >= game.enemyFireDelayMs) {
    game.enemyFireArmed = false;
    spawnEnemyShot();
  }

  for (Bullet &shot : game.enemyShots) {
    if (!shot.active) {
      continue;
    }

    shot.x -= game.enemyShotSpeed;
    if (shot.x < 0) {
      shot.active = false;
    }
  }
}

void checkCollisions() {
  if (game.playerShotActive) {
    const int dx = game.playerShotX - kEnemyX;
    const int dy = game.playerShotY - game.enemyY;
    if ((dx * dx) + (dy * dy) <= (game.enemyRadius * game.enemyRadius)) {
      game.playerShotActive = false;
      ++game.score;
      tone(kBuzzerPin, 500, 20);
    }
  }

  const int playerCenterY = game.playerY + (kPlayerHeight / 2);
  for (Bullet &shot : game.enemyShots) {
    if (!shot.active) {
      continue;
    }

    const bool hitX = shot.x >= kPlayerX && shot.x <= (kPlayerX + kPlayerWidth);
    const bool hitY = abs(shot.y - playerCenterY) <= (shot.radius + (kPlayerHeight / 2));
    if (hitX && hitY) {
      shot.active = false;
      --game.lives;
      tone(kBuzzerPin, 100, 100);
      if (game.lives <= 0) {
        game.gameOver = true;
      }
    }
  }
}

void renderGame(const Controls &controls) {
  if (!renderState.initialized) {
    tft.fillScreen(kColorBg);
    drawStars();
    drawControlPanel(controls);
    drawHud();
    renderState.initialized = true;
    renderState.controls = controls;
    renderState.controlsValid = true;
    renderState.hudValid = true;
  } else {
    tft.fillRect(kPlayerX, renderState.playerY, kShipBitmapWidth, kShipBitmapHeight, kColorBg);
    tft.fillCircle(kEnemyX, renderState.enemyY, renderState.enemyRadius + 1, kColorBg);

    if (renderState.playerShotActive) {
      tft.drawFastHLine(renderState.playerShotX, renderState.playerShotY, 10, kColorBg);
    }

    for (const Bullet &oldShot : renderState.enemyShots) {
      if (!oldShot.active) {
        continue;
      }
      tft.fillCircle(oldShot.x, oldShot.y, oldShot.radius + 1, kColorBg);
    }

    drawStars();

    if (!renderState.controlsValid || controlsChanged(renderState.controls, controls)) {
      drawControlPanel(controls);
      renderState.controls = controls;
      renderState.controlsValid = true;
    }

    const uint32_t nowSec = game.now / 1000UL;
    if (!renderState.hudValid ||
        renderState.score != game.score ||
        renderState.lives != game.lives ||
        renderState.level != game.level ||
        renderState.timeSec != nowSec) {
      drawHud();
      renderState.hudValid = true;
      renderState.score = game.score;
      renderState.lives = game.lives;
      renderState.level = game.level;
      renderState.timeSec = nowSec;
    }
  }

  tft.drawBitmap(kPlayerX, game.playerY, kShipBitmap, kShipBitmapWidth, kShipBitmapHeight, kColorShip);
  tft.fillCircle(kEnemyX, game.enemyY, game.enemyRadius, kColorEnemy);
  tft.fillCircle(kEnemyX + 4, game.enemyY + 6, game.enemyRadius / 3, kColorEnemyCore);

  if (game.playerShotActive) {
    tft.drawFastHLine(game.playerShotX, game.playerShotY, 10, ST77XX_WHITE);
  }

  for (const Bullet &shot : game.enemyShots) {
    if (!shot.active) {
      continue;
    }
    tft.drawCircle(shot.x, shot.y, shot.radius, ST77XX_WHITE);
  }

  renderState.playerY = game.playerY;
  renderState.enemyY = game.enemyY;
  renderState.enemyRadius = game.enemyRadius;
  renderState.playerShotActive = game.playerShotActive;
  renderState.playerShotX = game.playerShotX;
  renderState.playerShotY = game.playerShotY;
  for (size_t i = 0; i < 4; ++i) {
    renderState.enemyShots[i] = game.enemyShots[i];
  }
  renderState.score = game.score;
  renderState.lives = game.lives;
  renderState.level = game.level;
  renderState.timeSec = game.now / 1000UL;
}

void setupDisplayAndTouch() {
  pinMode(kTftBacklightPin, OUTPUT);
  digitalWrite(kTftBacklightPin, HIGH);

  gSpi.begin(kTftSckPin, kTftMisoPin, kTftMosiPin, -1);
  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(kScreenRotation);
  tft.invertDisplay(false);
  tft.fillScreen(kColorBg);

  touch.begin(gSpi);
  touch.setRotation(kScreenRotation);
  touchReady = true;

  Serial.printf("[DISPLAY] ST7789 SCK=%d MOSI=%d MISO=%d CS=%d DC=%d BL=%d\r\n",
                kTftSckPin, kTftMosiPin, kTftMisoPin, kTftCsPin, kTftDcPin, kTftBacklightPin);
  Serial.printf("[TOUCH] XPT2046 CS=%d IRQ=%d mapX=[%u,%u] mapY=[%u,%u]\r\n",
                kTouchCsPin, kTouchIrqPin, kTouchMinX, kTouchMaxX, kTouchMinY, kTouchMaxY);
}

void waitForStart() {
  drawIntro();
  const unsigned long startMs = millis();
  while (true) {
    uint16_t x = 0;
    uint16_t y = 0;
    const bool touched = readTouch(x, y);
    const bool firedByIr = (pollIrAction() == IrAction::Fire);
    const bool timeout = (millis() - startMs) > 6000UL;
    if (touched || firedByIr || timeout) {
      break;
    }
    delay(10);
  }

  beep(kNoteA4, 500);
  beep(kNoteA4, 500);
  beep(kNoteA4, 500);
  beep(kNoteF4, 350);
  beep(kNoteC5, 150);
  beep(kNoteA4, 500);
  beep(kNoteF4, 350);
  beep(kNoteC5, 150);
  beep(kNoteA4, 650);
  resetGame();
}

void waitForRestart() {
  static bool played = false;
  if (!played) {
    tone(kBuzzerPin, 200, 300);
    delay(300);
    tone(kBuzzerPin, 250, 200);
    delay(200);
    tone(kBuzzerPin, 300, 300);
    delay(300);
    played = true;
  }

  drawGameOverScreen();

  uint16_t x = 0;
  uint16_t y = 0;
  const bool touched = readTouch(x, y);
  const bool firedByIr = (pollIrAction() == IrAction::Fire);
  static unsigned long gameOverShownAt = 0;
  if (gameOverShownAt == 0) {
    gameOverShownAt = millis();
  }
  const bool timeout = (millis() - gameOverShownAt) > 8000UL;

  if (!(touched || firedByIr || timeout)) {
    delay(10);
    return;
  }

  tone(kBuzzerPin, 280, 300);
  delay(300);
  tone(kBuzzerPin, 250, 200);
  delay(200);
  tone(kBuzzerPin, 370, 300);
  delay(300);
  played = false;
  gameOverShownAt = 0;
  resetGame();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(kBuzzerPin, OUTPUT);
  randomSeed(static_cast<uint32_t>(esp_random()));

  setupDisplayAndTouch();

  irrecv.enableIRIn();
  Serial.printf("[IR] receiver enabled on GPIO%d\r\n", kIrRecvPin);

  waitForStart();
}

void loop() {
  game.now = millis();

  if (game.gameOver) {
    waitForRestart();
    return;
  }

  if ((game.now - lastFrameMs) < kFrameTimeMs) {
    delay(1);
    return;
  }
  lastFrameMs = game.now;

  Controls controls = readControls();
  if (controls.up) {
    game.playerY = clampPlayerY(game.playerY - 6);
  }
  if (controls.down) {
    game.playerY = clampPlayerY(game.playerY + 6);
  }
  if (controls.fire) {
    firePlayerShot();
  }

  updateDifficulty();
  updateEnemy();
  updatePlayerShot();
  updateEnemyShots();
  checkCollisions();
  renderGame(controls);
}
