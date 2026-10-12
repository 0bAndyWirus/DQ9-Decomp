#include "dsr/spu.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

int main() {
    dsr::Spu spu;
    std::array<std::uint8_t, 4096> memory{};
    std::uint32_t max_read_address=0;
    spu.read8 = [&](std::uint32_t a) {
        if(a>max_read_address) max_read_address=a;
        return memory[a % memory.size()];
    };
    spu.write8 = [&](std::uint32_t a, std::uint8_t v) { memory[a % memory.size()] = v; };
    for(std::size_t i=0;i<memory.size();i+=2) { memory[i]=0; memory[i+1]=0x01; }

    // PCM16 one-shot verifies sample fetch, gain, and asymmetric pan math.
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000404, 0, 32);
    spu.io_write(0x04000408, 0xfffe, 16);
    spu.io_write(0x0400040c, 1024, 32);
    spu.io_write(0x04000400, 0x80000000U | (32U<<16U) | (1U<<29U) | 127U, 32);
    spu.run(1024);
    std::array<std::int16_t, 2> frame{};
    assert(spu.drain(frame.data(), 1) == 1);
    assert(frame[0] == 128 && frame[1] == 64);
    const auto debug = spu.debug_state();
    assert(debug.channels[0].format == 1 && debug.channels[0].volume == 127);
    assert(debug.channels[0].pan == 32 && debug.channels[0].busy);
    spu.set_channel_muted(0, true);
    assert(spu.debug_state().channels[0].muted);
    spu.run(1024);
    assert(spu.drain(frame.data(), 1) == 1 && frame[0] == 0 && frame[1] == 0);
    spu.set_channel_muted(0, false);
    spu.set_solo_channel(1);
    spu.run(1024);
    assert(spu.drain(frame.data(), 1) == 1 && frame[0] == 0 && frame[1] == 0);
    spu.set_solo_channel(-1);

    // ADPCM header predictor and zero nibble decode are hand-computable.
    spu.reset();
    memory.fill(0);
    memory[0]=0x00; memory[1]=0x10; memory[2]=0; memory[3]=0; memory[4]=0x00;
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0xfffc, 16);
    spu.io_write(0x0400040c, 1024, 32);
    spu.io_write(0x04000400, 0x80000000U | (127U<<16U) | (2U<<29U) | 127U, 32);
    spu.run(1024);
    assert(spu.drain(frame.data(),1)==1);
    assert(frame[0] == 0 && frame[1] == 4096);
    // ADPCM nibble deltas use the reference table's single division by eight.
    spu.reset();
    memory.fill(0);
    memory[4]=0x03; // code 3 at the first nibble, step index 0 (step 7)
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0xfe00, 16);
    spu.io_write(0x0400040c, 4, 32);
    spu.io_write(0x04000400, 0x80000000U | (127U<<16U) | (2U<<29U) | 127U, 32);
    spu.io_write(0x04000510, 128, 32);
    spu.io_write(0x04000514, 64, 32);
    spu.io_write(0x04000508, 0x82, 8); // capture channel 0
    spu.io_write(0x04000418, 0xfe00, 16); // capture 0 clock from channel 1
    spu.run(1024);
    assert(memory[128]==6 && memory[129]==0);
    // The ADPCM header's step index (byte 2) seeds the first decode: index 16 is step 34, (2*3+1)*34/8 = 29.
    spu.reset();
    memory.fill(0);
    memory[2]=16; memory[4]=0x03;
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0xfe00, 16);
    spu.io_write(0x0400040c, 4, 32);
    spu.io_write(0x04000400, 0x80000000U | (127U<<16U) | (2U<<29U) | 127U, 32);
    spu.io_write(0x04000510, 128, 32);
    spu.io_write(0x04000514, 64, 32);
    spu.io_write(0x04000508, 0x82, 8);
    spu.io_write(0x04000418, 0xfe00, 16);
    spu.run(1024);
    assert(memory[128]==29 && memory[129]==0);
    // Channels 8-13 also decode PCM/ADPCM when format 2 is selected.
    spu.reset();
    memory.fill(0);
    memory[0]=0xfc; memory[1]=0x13; // predictor 0x13fc
    memory[4]=0x03;
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000488, 0xfffc, 16);
    spu.io_write(0x0400048c, 1024, 32);
    spu.io_write(0x04000480, 0x80000000U | (127U<<16U) | (2U<<29U) | 127U, 32);
    spu.run(1024);
    assert(spu.drain(frame.data(),1)==1 && frame[0]==0 && frame[1]==5120);


    // PSG channel duty produces both positive and negative plateaus.
    spu.reset();
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408+8*16, 0xfc00, 16); // one duty step per 2048 ticks
    spu.io_write(0x04000400+8*16, 0x80000000U | (3U<<29U) | (3U<<24U) | (64U<<16U) | 127U, 32);
    spu.run(32*1024);
    std::array<std::int16_t, 64> psg{};
    assert(spu.drain(psg.data(), 32)==32);
    bool psg_high=false, psg_low=false;
    for(std::size_t i=0;i<psg.size();i+=2) { psg_high|=psg[i]>0; psg_low|=psg[i]<0; }
    assert(psg_high && psg_low);

    // Capture unit writes little-endian mixed output bytes to guest memory.
    spu.reset();
    for(std::size_t i=0;i<512;i+=2) { memory[i]=0; memory[i+1]=1; }
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000404, 0, 32);
    spu.io_write(0x04000408, 0xfffe, 16);
    spu.io_write(0x0400040c, 1024, 32);
    spu.io_write(0x04000400, 0x80000000U | (127U<<16U) | (1U<<29U) | 127U, 32);
    spu.io_write(0x04000510, 128, 32);
    spu.io_write(0x04000514, 64, 32);
    spu.io_write(0x04000508, 0x82, 8); // start capture 0 from channel 0
    spu.io_write(0x04000418, 0xfe00, 16);
    spu.run(1024);
    assert(memory[128] != 0 || memory[129] != 0);
    // Noise uses the channel LFSR and changes its signed output over time.
    spu.reset();
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408+14*16, 0xfffd, 16);
    spu.io_write(0x04000400+14*16, 0x80000000U | (3U<<29U) | (64U<<16U) | 127U, 32);
    spu.run(10*1024);
    std::array<std::int16_t, 20> noise{};
    assert(spu.drain(noise.data(), 10)==10);
    bool noise_changed=false;
    for(std::size_t i=2;i<noise.size();i+=2) noise_changed|=noise[i]!=noise[0] || noise[i+1]!=noise[1];
    assert(noise_changed);
    // The reference 15-bit noise LFSR uses bit 0 as the output and taps
    // bits 0 and 14 via the conditional 0x6000 XOR.
    spu.reset();
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408+14*16, 0xfe00, 16); // one LFSR step per output sample
    spu.io_write(0x04000400+14*16, 0x80000000U | (3U<<29U) | (64U<<16U) | 127U, 32);
    std::array<std::int16_t, 32> expected_noise{};
    std::uint32_t expected_lfsr=0x7fffU;
    for(std::size_t i=0;i<expected_noise.size();i+=2) {
        const std::int32_t sample=(expected_lfsr&1U)!=0U?-32768:32767;
        const auto scale=[](std::int32_t value,unsigned multiplier) {
            return multiplier==127U?value:(value*static_cast<std::int32_t>(multiplier))>>7U;
        };
        const auto to_output=[](std::int32_t value) {
            value>>=6U;
            return static_cast<std::int16_t>(std::clamp(value,-512,511)*64);
        };
        expected_noise[i]=to_output(scale(sample,63U));
        expected_noise[i+1]=to_output(scale(sample,64U));
        if((expected_lfsr&1U)!=0U) expected_lfsr=(expected_lfsr>>1U)^0x6000U;
        else expected_lfsr>>=1U;
    }
    spu.run(16*1024);
    std::array<std::int16_t, 32> actual_noise{};
    assert(spu.drain(actual_noise.data(),16)==16);
    assert(actual_noise==expected_noise);

    // PCM one-shot clears busy, while loop mode remains busy past its end.
    spu.reset();
    spu.io_write(0x04000408, 0xffff, 16);
    spu.io_write(0x0400040c, 4, 32);
    spu.io_write(0x04000400, 0x80000000U | (127U<<16U) | 127U, 32);
    spu.run(34);
    assert((spu.io_read(0x04000400,32)&0x80000000U)==0U);
    spu.io_write(0x04000400, 0x80000000U | (1U<<27U) | (127U<<16U) | 127U, 32);
    spu.run(34);
    assert((spu.io_read(0x04000400,32)&0x80000000U)!=0U);

    // PNT is an introductory region and LEN extends its one-shot duration.
    spu.reset();
    spu.io_write(0x04000408, 0x0003ffff, 32);
    spu.io_write(0x0400040c, 1, 32);
    spu.io_write(0x04000400, 0x80000000U | (127U<<16U) | 127U, 32);
    spu.run(10);
    assert((spu.io_read(0x04000400,32)&0x80000000U)!=0U);
    spu.run(22);
    assert((spu.io_read(0x04000400,32)&0x80000000U)!=0U);
    spu.run(2);
    assert((spu.io_read(0x04000400,32)&0x80000000U)==0U);

    // Hold preserves the last sample after a one-shot clears busy.
    spu.reset();
    for(std::size_t i=0;i<16;i+=2) { memory[i]=0; memory[i+1]=1; }
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0xffff, 16);
    spu.io_write(0x0400040c, 4, 32);
    spu.io_write(0x04000400, 0x80000000U | (1U<<29U) | (1U<<15U) |
                                  (127U<<16U) | 127U, 32);
    spu.run(1024);
    assert((spu.io_read(0x04000400,32)&0x80000000U)==0U);
    assert(spu.drain(frame.data(),1)==1 && frame[1]==256);

    // Updating a busy channel with Start still set must not restart playback.
    spu.reset();
    memory[0]=0; memory[1]=1; memory[2]=0; memory[3]=2;
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0xfe00, 16);
    spu.io_write(0x0400040c, 4, 32);
    const std::uint32_t looping_pcm=0x80000000U | (1U<<27U) | (1U<<29U) |
                                    (127U<<16U) | 127U;
    spu.io_write(0x04000400, looping_pcm, 32);
    spu.run(1024);
    assert(spu.drain(frame.data(),1)==1 && frame[1]==256);
    spu.io_write(0x04000400, (looping_pcm&~127U)|63U, 32);
    spu.run(1024);
    assert(spu.drain(frame.data(),1)==1 && frame[1]==192);

    // ADPCM intro PNT may exceed its loop LEN without reading past the buffer.
    spu.reset();
    memory.fill(0);
    max_read_address=0;
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0x0003ffff, 32);
    spu.io_write(0x0400040c, 1, 32);
    spu.io_write(0x04000400, 0x80000000U | (1U<<27U) | (2U<<29U) |
                                  (127U<<16U) | 127U, 32);
    spu.run(1024);
    assert(max_read_address==15U);

    // Volume divider 3 is a 16x attenuation, not a 4x attenuation.
    spu.reset();
    memory.fill(0x7f);
    spu.io_write(0x04000500, 0x807f, 32);
    spu.io_write(0x04000408, 0xffff, 16);
    spu.io_write(0x0400040c, 4, 32);
    spu.io_write(0x04000400, 0x80000000U | (1U<<27U) | (1U<<29U) |
                                  (127U<<16U) | (3U<<8U) | 127U, 32);
    spu.run(1024);
    assert(spu.drain(frame.data(),1)==1 && frame[1]==1984);

    // Capture clocks are channel 1/3 timer overflows, independent of both
    // playback busy and the 1024-tick output DAC. Check every byte write.
    for(unsigned unit=0;unit<2U;++unit) {
        spu.reset();
        memory.fill(0);
        std::vector<std::pair<std::uint32_t,std::uint8_t>> writes;
        spu.write8=[&](std::uint32_t a,std::uint8_t v) { writes.emplace_back(a,v); };
        const std::uint32_t destination=0x04000510U+unit*8U;
        const std::uint32_t clock=0x04000418U+unit*32U;
        spu.io_write(destination,128,32);
        spu.io_write(destination+4U,1,32); // one word = two PCM16 samples
        spu.io_write(clock,0xff00,16); // one sample per 512 ticks
        spu.io_write(0x04000508U+unit,0x86,8); // direct source, one shot
        spu.run(511);
        assert(writes.empty());
        spu.run(1);
        assert(writes.size()==2U && writes[0].first==128U && writes[1].first==129U);
        assert((spu.io_read(0x04000508U+unit,8)&0x80U)!=0U);
        spu.run(512);
        assert(writes.size()==4U && writes[2].first==130U && writes[3].first==131U);
        assert((spu.io_read(0x04000508U+unit,8)&0x80U)==0U);
        spu.run(1024);
        assert(writes.size()==4U);

        writes.clear();
        spu.io_write(destination+4U,0,32); // zero length behaves as one word
        spu.io_write(clock,0xfc00,16); // one sample per 2048 ticks
        spu.io_write(0x04000508U+unit,0x8a,8); // PCM8 loop
        spu.run(2047);
        assert(writes.empty());
        spu.run(1);
        assert(writes.size()==1U && writes[0].first==128U);
        spu.run(4*2048);
        assert(writes.size()==5U && writes[3].first==131U && writes[4].first==128U);
        assert((spu.io_read(0x04000508U+unit,8)&0x80U)!=0U);
        spu.io_write(0x04000508U+unit,0,8);
        spu.run(2048);
        assert(writes.size()==5U);
    }

    // Sub-DAC capture events observe distinct source samples, not repeated
    // copies of the sample at the end of the 1024-tick output interval.
    spu.reset();
    memory.fill(0);
    memory[0]=1; memory[1]=2; memory[2]=3; memory[3]=4;
    spu.write8=[&](std::uint32_t a,std::uint8_t v) { memory[a%memory.size()]=v; };
    spu.io_write(0x04000408,0xff80,16); // PCM8 every 256 ticks
    spu.io_write(0x0400040c,4,32);
    spu.io_write(0x04000400,0x80000000U | (1U<<27U) | 127U,32);
    spu.io_write(0x04000418,0xff80,16); // capture 0 every 256 ticks
    spu.io_write(0x04000510,128,32);
    spu.io_write(0x04000514,1,32);
    spu.io_write(0x04000508,0x8e,8); // PCM8 direct source, one shot
    spu.run(1024);
    assert(memory[128]==1 && memory[129]==2 && memory[130]==3 && memory[131]==4);

    return 0;
}
