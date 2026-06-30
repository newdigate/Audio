// EVKB_WM8960_Sine - NXP MIMXRT1060-EVKB audio hardware test.
//
// Plays a 440 Hz sine out SAI1 I2S (MCU as I2S master) to the on-board WM8960
// codec (AudioControlWM8960, I2C1 @ 0x1A) -> headphone / line out. Hardware-verified
// on a real EVKB. Status on Serial6 (the EVKB's OpenSDA virtual COM, 115200) - the
// EVKB exposes the debug-probe COM as Serial6, not Serial.
//
// Requires the EVKB-aware teensy4 core and this (newdigate) Audio fork.
#include <Audio.h>

AudioSynthWaveformSine sine1;      // tone generator
AudioOutputI2S         i2s1;       // SAI1 I2S out (master) -> WM8960
AudioControlWM8960     wm8960;     // on-board WM8960 codec (I2C1 @ 0x1A)
AudioConnection        pc1(sine1, 0, i2s1, 0);   // -> left
AudioConnection        pc2(sine1, 0, i2s1, 1);   // -> right

bool wmOk;

void setup() {
  Serial6.begin(115200);
  uint32_t t = millis();
  while (!Serial6 && (millis() - t) < 2000) ;
  Serial6.println("\n\n=== EVKB WM8960 sine test ===");

  AudioMemory(12);
  wmOk = wm8960.enable();                     // I2C codec bring-up (WM8960 as I2S slave)
  Serial6.printf("WM8960 enable: %s\n", wmOk ? "OK" : "FAIL");
  wm8960.volume(0.6f);                        // headphone OUT
  wm8960.speakerVolume(0.7f);                 // class-D speaker OUT

  sine1.frequency(440.0f);
  sine1.amplitude(0.5f);
  Serial6.println("playing 440 Hz sine -> WM8960 (headphone / line out)");
}

void loop() {
  // cpu(x100) = AudioProcessorUsage() * 100, so 0.50% prints as 50. Non-zero => the
  // audio update ISR is firing (the SAI1 I2S DMA is clocking the engine).
  Serial6.printf("running: WM8960=%s  mem=%u blk  cpu(x100)=%d\n",
                 wmOk ? "OK" : "FAIL", AudioMemoryUsageMax(),
                 (int)(AudioProcessorUsage() * 100.0f));
  Serial6.flush();
  delay(2000);
}
