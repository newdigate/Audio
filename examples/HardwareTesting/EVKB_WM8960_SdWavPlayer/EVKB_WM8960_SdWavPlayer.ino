// EVKB_WM8960_SdWavPlayer - NXP MIMXRT1060-EVKB audio hardware test.
//
// microSD (uSDHC1 / BUILTIN_SDCARD) -> AudioPlaySdWav -> SAI1 I2S -> on-board WM8960
// codec -> headphone / line out. Auto-mounts the card, lists its files, and plays the
// first .WAV it finds. Hardware-verified on a real EVKB. Status on Serial6 (the EVKB's
// OpenSDA virtual COM, 115200).
//
// Put a 16-bit PCM WAV, 44.1 kHz (mono or stereo) in the card root (e.g. TEST.WAV) -
// that is the format AudioPlaySdWav requires; MP3 / 24-bit / 48 kHz will not play.
//
// Requires the EVKB-aware teensy4 core, this (newdigate) Audio fork, and the newdigate
// SdFat fork (which powers the EVKB microSD load switch on core pin 7).
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

AudioPlaySdWav     playWav1;
AudioOutputI2S     i2s1;
AudioControlWM8960 wm8960;
AudioConnection    pc1(playWav1, 0, i2s1, 0);
AudioConnection    pc2(playWav1, 1, i2s1, 1);

static char wavName[64] = "";
static bool g_sdOk = false;
static int  g_fileCount = 0;

static bool endsWithWav(const char *n) {
  size_t L = strlen(n);
  return L > 4 && (strcasecmp(n + L - 4, ".wav") == 0);
}

void setup() {
  Serial6.begin(115200);
  uint32_t t = millis();
  while (!Serial6 && (millis() - t) < 2000) ;
  Serial6.println("\n\n=== EVKB SD WAV player ===");

  AudioMemory(30);
  bool wmOk = wm8960.enable();
  wm8960.volume(0.6f);
  Serial6.printf("WM8960 enable: %s\n", wmOk ? "OK" : "FAIL");

  g_sdOk = SD.begin(BUILTIN_SDCARD);
  Serial6.printf("SD.begin(BUILTIN_SDCARD): %s\n", g_sdOk ? "OK" : "FAIL (no card / wiring?)");
  if (!g_sdOk) { Serial6.flush(); return; }

  File root = SD.open("/");
  Serial6.println("files on card:");
  while (true) {
    File f = root.openNextFile();
    if (!f) break;
    g_fileCount++;
    Serial6.printf("  %-24s %10lu B%s\n", f.name(), (unsigned long)f.size(),
                   f.isDirectory() ? "  <dir>" : "");
    if (!wavName[0] && !f.isDirectory() && endsWithWav(f.name()))
      strncpy(wavName, f.name(), sizeof(wavName) - 1);
    f.close();
  }
  root.close();

  if (wavName[0]) {
    Serial6.printf("playing: %s\n", wavName);
    playWav1.play(wavName);
    delay(10);   // let playback start
  } else {
    Serial6.println("no .WAV file found on card");
  }
  Serial6.flush();
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    // pos advancing in real time + playing=1 => genuine playback.
    Serial6.printf("SD=%s files=%d wav=[%s] playing=%d pos=%lu cpu(x100)=%d mem=%u\n",
                   g_sdOk ? "OK" : "FAIL", g_fileCount, wavName[0] ? wavName : "none",
                   playWav1.isPlaying(), (unsigned long)playWav1.positionMillis(),
                   (int)(AudioProcessorUsage() * 100.0f), AudioMemoryUsageMax());
  }
}
