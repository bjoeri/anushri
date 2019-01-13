// Copyright 2012 Olivier Gillet.
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "anu/drum_synth.h"

#include "avrlib/op.h"
#include "avrlib/random.h"
#include "avrlib/time.h"

#include "anu/audio_buffer.h"
#include "anu/dsp_utils.h"
#include "anu/resources.h"

namespace anu {

using namespace avrlib;

/* extern */
DrumSynth drum_synth;

/* static */
DrumPatch DrumSynth::patch_[kNumDrumInstruments];

/* static */
DrumState DrumSynth::state_[kNumDrumInstruments];

/* static */
uint8_t DrumSynth::sample_rate_ = 0;

/* static */
uint8_t DrumSynth::sample_counter_;

/* static */
uint8_t DrumSynth::sample_;

/* static */
uint8_t DrumSynth::fade_counter_;

/* static */
bool DrumSynth::playing_;

/* static */
uint32_t DrumSynth::last_event_time_;

static const prog_uint8_t preset_bd_1[] PROGMEM = { 60, 18, 104, 120, 0 };
static const prog_uint8_t preset_bd_2[] PROGMEM = { 56, 60, 120, 150, 0 };
static const prog_uint8_t preset_bd_3[] PROGMEM = { 60, 42, 130, 180, 14 };
static const prog_uint8_t preset_bd_4[] PROGMEM = { 72, 20, 66, 224, 0 };
static const prog_uint8_t preset_bd_5[] PROGMEM = { 42, 52, 106, 160, 60 };

static const prog_uint8_t preset_sd_1[] PROGMEM = { 108, 18, 16, 72, 64 };
static const prog_uint8_t preset_sd_2[] PROGMEM = { 108, 36, 32, 96, 140 };
static const prog_uint8_t preset_sd_3[] PROGMEM = { 108, 36, 50, 90, 180 };
static const prog_uint8_t preset_sd_4[] PROGMEM = { 116, 36, 32, 80, 150 };
static const prog_uint8_t preset_sd_5[] PROGMEM = { 124, 40, 190, 90, 40 };

/*
* Hi-hat synthesis is done in the style of YM3812/OPL2 and is derived from its  
* MAME emulation (fmopl.cpp). Hi-hat operator frequencies has been derived from 
* actual observations of Yamaha PSS-460 samples and OPL2 emulation comparisons.
*
* 1st Hi-hat operator should have a frequency of ~508Hz (Yamaha PSS-460)
* 2nd Hi-hat operator frequency is hard-wired to 2/3 of 1st operator frequency 
* For optimization reasons the hi-hat presets should contain the index for the 
* 2nd operator phase increment, e.g. index ~132 for ~508*2/3 Hz (569 phase inc).
* Crunchiness is used to set level of noise (max for hi-hat and min for cymbal).
*/
static const prog_uint8_t preset_hh_1[] PROGMEM = { 132, 0, 0, 80, 255 };
static const prog_uint8_t preset_hh_2[] PROGMEM = { 134, 0, 0, 80, 255 };
static const prog_uint8_t preset_hh_3[] PROGMEM = { 134, 0, 0, 90, 32 };
static const prog_uint8_t preset_hh_4[] PROGMEM = { 134, 0, 0, 90, 255 };
static const prog_uint8_t preset_hh_5[] PROGMEM = { 134, 0, 0, 45, 255 };

static const prog_uint8_t* drum_presets[] = {
  preset_bd_1,
  preset_bd_2,
  preset_bd_3,
  preset_bd_4,
  preset_bd_5,

  preset_sd_1,
  preset_sd_2,
  preset_sd_3,
  preset_sd_4,
  preset_sd_5,

  preset_hh_1,
  preset_hh_2,
  preset_hh_3,
  preset_hh_4,
  preset_hh_5,
};

/* static */
void DrumSynth::Init() {
  memset(state_, 0, sizeof(DrumState) * kNumDrumInstruments);
}

/* static */
void DrumSynth::Trigger(uint8_t instrument, uint8_t level) {
  last_event_time_ = milliseconds();
  
  // Reset all phases.
  state_[instrument].phase = 0;
  state_[instrument].pitch_env_phase = 0;
  state_[instrument].amp_env_phase = 0;
  
  // Initialize envelope increments
  state_[instrument].pitch_env_increment = pgm_read_word(
      lut_res_drm_env_increments + patch_[instrument].pitch_decay);
  state_[instrument].amp_env_increment = pgm_read_word(
      lut_res_drm_env_increments + patch_[instrument].amp_decay);
  state_[instrument].level = U8U8MulShift8(level, patch_[instrument].level);
  playing_ = true;
}

/* static */
void DrumSynth::MorphPatch(uint8_t instrument, uint8_t value) {
  uint8_t offset = instrument * 5 + (value >> 6);
  uint8_t balance = value << 2;
  const prog_uint8_t* a = drum_presets[offset];
  const prog_uint8_t* b = drum_presets[offset + 1];
  uint8_t* address = (uint8_t*)(&patch_[instrument].pitch);
  for (uint8_t i = 0; i < 5; ++i) {
    address[i] = U8Mix(pgm_read_byte(a + i), pgm_read_byte(b + i), balance);
  }
}

static const prog_uint8_t drums_cc_map[] PROGMEM = {
  // BD
  0, 1, 2, 3, 4, 5,
  // SD
  6, 7, 8, 9, 10, 11,
  // HH
#if 1 //BER:TODO: Add more parameter(s) for new OPL2 (YM3812) style HH
  12, 15, 17
#endif
};

/* static */
void DrumSynth::SetParameterCc(uint8_t cc, uint8_t value) {
  if (cc < 16 || cc > 30) {
    return;
  }
  uint8_t address = pgm_read_byte(drums_cc_map + cc - 16);
  uint8_t* data = static_cast<uint8_t*>(static_cast<void*>(patch_));
  data[address] = value << 1;
}

/* static */
void DrumSynth::SetBandwidth(uint8_t bandwidth) {
  bandwidth = ~bandwidth;
  // sample_rate = U8U8MulShift8(sample_rate, sample_rate);
  sample_rate_ = bandwidth >> 3;
}

/* static */
void DrumSynth::SetBalance(uint8_t mix) {
  if (mix < 128) {
    patch_[0].level = 255;
    patch_[1].level = mix << 1;
  } else {
    patch_[0].level = ~((mix - 128) << 1);
    patch_[1].level = 255;
  }
  patch_[2].level = patch_[1].level >> 1;
}

/* static */
void DrumSynth::FillWithSilence() {
  if (sample_) {
    if (fade_counter_) {
      --fade_counter_;
    } else {
      fade_counter_ = 255;
      --sample_;
    }
  }
  while (audio_buffer.writable()) {
    audio_buffer.Overwrite(sample_);
  }
}

/* static */
void DrumSynth::Render() {
  uint8_t sample = sample_;
  uint8_t sample_counter = sample_counter_;
  while (audio_buffer.writable() >= kAudioBlockSize) {
    UpdateModulations();
    uint8_t noise = Random::state_msb();
    uint16_t phase_0 = state_[0].phase;
    uint16_t phase_1 = state_[1].phase;
    uint16_t phase_2 = state_[2].phase;
    uint16_t phase_2b = state_[2].pitch_env_phase; // pitch_env_phase used as 2nd phase for hi-hat
    const int8_t hhnoisesample = 120 - S8U8MulShift8(80, state_[2].amp_level_noise); // modulation of noise level for Hi-hat/Cymbal morph
    for (uint8_t i = 0; i < kAudioBlockSize; ++i) {
      ++sample_counter;
      int16_t mix = 128;
      noise = (noise * 73) + 1;

      phase_0 += state_[0].phase_increment;
      phase_1 += state_[1].phase_increment;
      phase_2 += state_[2].phase_increment;
      phase_2b += state_[2].pitch_env_increment; // pitch_env_increment updates 2nd phase for hi-hat
      
      // Linear interpolation optimized for the case when the delta
      // between adjacent samples is in the -127..+127 range.
      Word bd_sample_pair;
      bd_sample_pair.value = pgm_read_word(wav_res_sine + (phase_0 >> 8));
      int8_t bd = bd_sample_pair.bytes[0];
      int8_t bd_2 = bd_sample_pair.bytes[1];
      bd += S8U8MulShift8(bd_2 - bd, phase_0);
      mix += S8U8MulShift8(bd, state_[0].amp_level);

      int8_t sd = pgm_read_byte(wav_res_sine + (phase_1 >> 8));
      mix += S8U8MulShift8(sd, state_[1].amp_level);
      mix += S8U8MulShift8(noise, state_[1].amp_level_noise);

      // Mimic OPL2/YM3812 style hi-hat
      const uint8_t hibits_phase_2 = phase_2 >> 8;
      const uint8_t bit2 = (hibits_phase_2 & 0x04);
      const uint8_t bit3 = (hibits_phase_2 & 0x08);
      const uint8_t bit7 = (hibits_phase_2 & 0x80);
      const uint8_t res1 = bit3 || ((bit2 || bit7) && !(bit2 && bit7)); // bit3 | bit2 ^ bit7
      const uint8_t hibits_phase_2b = phase_2b >> 8;
      const uint8_t bit3e = (hibits_phase_2b & 0x08);
      const uint8_t bit5e = (hibits_phase_2b & 0x20);
      const uint8_t res2 = ((bit3e || bit5e) && !(bit3e && bit5e)); // bit3e ^ bit5e
      int8_t hhsample;
      if (res2 || res1) {
        if (noise & 0x1) {
          hhsample = -hhnoisesample; // OPL2 sinlookup = 0x2d0 with noise
        }
        else {
          hhsample = -120; // OPL2 sinlookup = 0x234
        }
      } 
      else {
        if (noise & 0x1) { 
          hhsample = hhnoisesample; // OPL2 sinlookup = 0x34 with noise
        }
        else {
          hhsample = 120; // OPL2 sinlookup = 0xd0
        }
      }
      
      mix += S8U8MulShift8(hhsample, state_[2].amp_level);
      
      if (sample_counter > sample_rate_) {
        if (mix > 255) mix = 255;
        if (mix < 0) mix = 0;
        sample = mix;
        sample_counter = 0;
      }
      audio_buffer.Overwrite(sample);
    }
    state_[0].phase = phase_0;
    state_[1].phase = phase_1;
    state_[2].phase = phase_2;
    state_[2].pitch_env_phase = phase_2b; // pitch_env_phase used as 2nd phase for hi-hat
  }
  sample_ = sample;
  sample_counter_ = sample_counter;
  fade_counter_ = 255;
}

/* static */
void DrumSynth::UpdateModulations() {
  playing_ = false;
  for (uint8_t i = 0; i < kNumDrumInstruments; ++i) {
    // Step amp envelope.
    state_[i].amp_env_phase += state_[i].amp_env_increment;
    if (state_[i].amp_env_phase < state_[i].amp_env_increment) {
      state_[i].amp_env_phase = 0xffff;
      state_[i].amp_env_increment = 0;
    }
    else {
      playing_ = true;
    }
    state_[i].amp_level = U8U8MulShift8(
        state_[i].level,
        InterpolateSample(wav_res_drm_envelope, state_[i].amp_env_phase));
    
    // Compute pitch
    uint16_t pitch = static_cast<uint16_t>(patch_[i].pitch) << 8;
    if (i == 0) { // add pitch crunchiness mod for BD
      pitch += U8U8Mul(Random::GetByte(), patch_[i].crunchiness);
    }
    if (i != 2) { // add pitch envelope mod for BD/SD
      state_[i].pitch_env_phase += state_[i].pitch_env_increment;
      if (state_[i].pitch_env_phase < state_[i].pitch_env_increment) {
        state_[i].pitch_env_phase = 0xffff;
        state_[i].pitch_env_increment = 0;
      }
      pitch += U8U8Mul(
        patch_[i].pitch_mod,
        InterpolateSample(wav_res_drm_envelope, state_[i].pitch_env_phase));
    }
    // Compute phase increment from pitch
    state_[i].phase_increment = InterpolateIncreasing(
        lut_res_drm_phase_increments,
        pitch);
    if (i == 2) {
      // pitch_env_increment used as 2nd phase for hi-hat, hardwired to 2/3 of 1st operator
      state_[i].pitch_env_increment = state_[i].phase_increment;
      state_[i].phase_increment = (state_[i].phase_increment * 3) / 2;
    }
  }
  state_[1].amp_level_noise = U8U8MulShift8(
      state_[1].amp_level,
      patch_[1].crunchiness);
  state_[1].amp_level = U8U8MulShift8(
      state_[1].amp_level,
      ~patch_[1].crunchiness);
  state_[2].amp_level_noise = patch_[2].crunchiness; // crunchiness for hi-hat noise
}

/* static */
uint32_t DrumSynth::idle_time_ms() {
  uint32_t now = milliseconds();
  return now - last_event_time_;
}

};  // namespace anu
