#include "dsr/spu.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

struct TestBus {
    std::array<std::uint8_t,8192> memory{};
    // One ordered trace includes both sample reads and capture writes.
    std::vector<std::array<std::uint32_t,3>> events;

    std::uint32_t read(std::uint32_t address) {
        const auto value=memory[address%memory.size()];
        events.push_back({0U,address,value});
        return value;
    }
    void write(std::uint32_t address,std::uint8_t value) {
        events.push_back({1U,address,value});
        memory[address%memory.size()]=value;
    }
};

// Frozen pre-deadline scalar behavior. In particular, every existing step adds
// to every busy channel's phase before consuming its events in channel order.
// Keep the original mixer arithmetic too, including its zero-sample work.
struct ScalarSpu {
    static constexpr std::array<int,89> step_table = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
    };
    static constexpr std::array<int,8> index_adjust={-1,-1,-1,-1,2,4,6,8};
    static constexpr std::array<std::array<int,8>,8> duty={{
        {{0,0,0,0,0,0,0,1}}, {{0,0,0,0,0,0,1,1}},
        {{0,0,0,0,0,1,1,1}}, {{0,0,0,0,1,1,1,1}},
        {{0,0,0,1,1,1,1,1}}, {{0,0,1,1,1,1,1,1}},
        {{0,1,1,1,1,1,1,1}}, {{0,0,0,0,0,0,0,0}}
    }};
    struct Channel {
        std::uint32_t cnt=0,sad=0,len=0;
        std::uint16_t timer=0,pnt=0;
        bool busy=false,holding=false;
        std::uint64_t phase=0;
        std::uint32_t pos=0;
        std::int32_t current=0,predictor=0,initial_predictor=0;
        int adpcm_index=0,initial_adpcm_index=0;
        std::uint32_t lfsr=0x7fff;
        unsigned duty_pos=0;
        std::uint32_t loop_pos=0;
        std::int32_t loop_predictor=0;
        int loop_index=0;
        bool loop_state_valid=false;
    };
    struct Capture {
        std::uint8_t cnt=0;
        std::uint32_t dst=0,len=0,written=0;
        std::uint64_t phase=0;
    };
    TestBus* bus=nullptr;
    std::array<Channel,16> ch{};
    std::array<Capture,2> cap{};
    std::array<bool,16> debug_muted{};
    int debug_solo=-1;
    std::uint16_t soundcnt=0,bias=0x0200;
    std::uint64_t sample_phase=0;
    std::array<std::int16_t,65536> output{};
    std::size_t output_read=0,output_write=0,output_count=0;

    void reset() {
        auto* old_bus=bus;
        *this=ScalarSpu{};
        bus=old_bus;
    }
    static std::uint32_t period(const Channel& c) {
        return std::max(1U,static_cast<std::uint32_t>(0x10000U-c.timer)*2U);
    }
    static std::uint32_t sample_count(const Channel& c) {
        const unsigned fmt=(c.cnt>>29U)&3U;
        const std::uint32_t words=static_cast<std::uint32_t>(c.pnt)+c.len;
        if(fmt==0U) return words*4U;
        if(fmt==1U) return words*2U;
        if(fmt==2U) return words*8U;
        return 0;
    }
    static std::int32_t scale7(std::int32_t value,unsigned multiplier) {
        return multiplier==127U?value:(value*static_cast<std::int32_t>(multiplier))>>7U;
    }
    static void finish(Channel& c) {
        c.busy=false;
        c.holding=(c.cnt&(1U<<15U))!=0U;
        if(!c.holding) c.current=0;
    }
    void start(Channel& c) {
        c.busy=true; c.holding=false; c.phase=0; c.pos=0; c.current=0; c.duty_pos=0;
        c.predictor=0; c.adpcm_index=0; c.loop_state_valid=false; c.lfsr=0x7fff;
        if(((c.cnt>>29U)&3U)==2U) {
            const auto lo=bus->read(c.sad)&255U;
            const auto hi=bus->read(c.sad+1U)&255U;
            c.predictor=static_cast<std::int16_t>(lo|(hi<<8U));
            c.current=c.predictor;
            c.adpcm_index=std::min(88,static_cast<int>(bus->read(c.sad+2U)&127U));
            c.initial_predictor=c.predictor; c.initial_adpcm_index=c.adpcm_index; c.pos=8U;
            if(c.pnt==0U) {
                c.loop_pos=0U; c.loop_predictor=c.predictor; c.loop_index=c.adpcm_index;
                c.loop_state_valid=true;
            }
        }
    }
    void decode_adpcm(Channel& c) {
        const std::uint32_t total=sample_count(c);
        if(c.pos>=total) {
            if(((c.cnt>>27U)&3U)!=1U) { finish(c); return; }
            const std::uint32_t loop_start=static_cast<std::uint32_t>(c.pnt)*8U;
            if(loop_start==0U) {
                c.pos=8U; c.predictor=c.initial_predictor; c.adpcm_index=c.initial_adpcm_index;
            } else {
                c.pos=loop_start;
                if(c.loop_state_valid && c.loop_pos==loop_start) {
                    c.predictor=c.loop_predictor; c.adpcm_index=c.loop_index;
                }
            }
        }
        if(c.pos>=total) { finish(c); return; }
        const std::uint32_t nibble=c.pos;
        const std::uint32_t loop_start=static_cast<std::uint32_t>(c.pnt)*8U;
        if(!c.loop_state_valid && nibble==loop_start) {
            c.loop_pos=nibble; c.loop_predictor=c.predictor; c.loop_index=c.adpcm_index;
            c.loop_state_valid=true;
        }
        const auto raw=bus->read(c.sad+nibble/2U)&255U;
        const int code=static_cast<int>((nibble&1U)?raw>>4U:raw&15U);
        const int step=step_table[static_cast<std::size_t>(c.adpcm_index)];
        const int diff=((code&7)*2+1)*step>>3;
        c.predictor=std::clamp(c.predictor+((code&8)!=0?-diff:diff),-32768,32767);
        c.adpcm_index=std::clamp(c.adpcm_index+index_adjust[static_cast<std::size_t>(code&7)],0,88);
        c.current=c.predictor; ++c.pos;
    }
    void run_events(Channel& c,unsigned n) {
        const std::uint64_t p=period(c);
        while(c.busy && c.phase>=p) {
            c.phase-=p;
            const unsigned fmt=(c.cnt>>29U)&3U;
            if(fmt==2U) { decode_adpcm(c); continue; }
            if(fmt==0U || fmt==1U) {
                const auto total=sample_count(c);
                if(c.pos>=total) {
                    if(((c.cnt>>27U)&3U)!=1U) { finish(c); break; }
                    const std::uint32_t loop_start=static_cast<std::uint32_t>(c.pnt)*(fmt==0U?4U:2U);
                    if(loop_start>=total) { finish(c); break; }
                    c.pos=loop_start;
                }
                if(fmt==0U) {
                    const auto b=bus->read(c.sad+c.pos)&255U;
                    c.current=static_cast<std::int8_t>(b)*256; ++c.pos;
                } else {
                    const auto lo=bus->read(c.sad+c.pos*2U)&255U;
                    const auto hi=bus->read(c.sad+c.pos*2U+1U)&255U;
                    c.current=static_cast<std::int16_t>(lo|(hi<<8U)); ++c.pos;
                }
            } else if(n<8U) c.current=0;
            else if(n<14U) {
                ++c.duty_pos; c.duty_pos&=7U;
                c.current=duty[(c.cnt>>24U)&7U][c.duty_pos]!=0?32767:-32768;
            } else {
                c.current=(c.lfsr&1U)!=0U?-32768:32767;
                if((c.lfsr&1U)!=0U) c.lfsr=(c.lfsr>>1U)^0x6000U;
                else c.lfsr>>=1U;
            }
        }
    }
    std::uint32_t io_read(std::uint32_t addr,int width) const {
        const auto base=addr&~3U;
        std::uint32_t value=0;
        if(base>=0x04000400U && base<0x04000500U) {
            const auto& c=ch[(base-0x04000400U)/16U];
            const auto off=(base-0x04000400U)%16U;
            if(off==0U) value=(c.cnt&0x7fffffffU)|(c.busy?0x80000000U:0U);
            else if(off==4U) value=c.sad;
            else if(off==8U) value=c.timer|(static_cast<std::uint32_t>(c.pnt)<<16U);
            else value=c.len;
        } else if(base==0x04000500U) value=soundcnt;
        else if(base==0x04000504U) value=bias;
        else if(base==0x04000508U) value=cap[0].cnt|(static_cast<std::uint32_t>(cap[1].cnt)<<8U);
        else if(base==0x04000510U) value=cap[0].dst;
        else if(base==0x04000514U) value=cap[0].len;
        else if(base==0x04000518U) value=cap[1].dst;
        else if(base==0x0400051cU) value=cap[1].len;
        const auto shift=(addr&3U)*8U;
        if(width==8) return (value>>shift)&255U;
        if(width==16) return (value>>shift)&65535U;
        return value;
    }
    void io_write(std::uint32_t addr,std::uint32_t value,int width) {
        const auto base=addr&~3U;
        const auto shift=(addr&3U)*8U;
        const std::uint32_t mask=width==8?255U:(width==16?65535U:0xffffffffU);
        const auto placed=width==32?value:(value&mask)<<shift;
        const auto pmask=width==32?mask:mask<<shift;
        if(base>=0x04000400U && base<0x04000500U) {
            auto& c=ch[(base-0x04000400U)/16U];
            const auto off=(base-0x04000400U)%16U;
            if(off==0U) {
                const bool was_busy=c.busy;
                c.cnt=(c.cnt&~pmask)|(placed&pmask);
                if((placed&pmask&0x80000000U)!=0U) {
                    if(!was_busy) start(c);
                } else if((pmask&0x80000000U)!=0U && c.busy) {
                    c.busy=false; c.holding=false; c.current=0;
                }
                c.cnt&=0x7fffffffU;
            } else if(off==4U) c.sad=((c.sad&~pmask)|(placed&pmask))&0x07fffffcU;
            else if(off==8U) {
                c.timer=static_cast<std::uint16_t>((c.timer&~(pmask&65535U))|(placed&pmask&65535U));
                c.pnt=static_cast<std::uint16_t>((c.pnt&~((pmask>>16U)&65535U))|((placed>>16U)&((pmask>>16U)&65535U)));
            } else c.len=((c.len&~pmask)|(placed&pmask))&0x003fffffU;
            return;
        }
        const auto merge=[&](std::uint32_t old) { return (old&~pmask)|(placed&pmask); };
        if(base==0x04000500U) soundcnt=static_cast<std::uint16_t>(merge(soundcnt)&0xbf7fU);
        else if(base==0x04000504U) bias=static_cast<std::uint16_t>(merge(bias)&0x03ffU);
        else if(base==0x04000508U) {
            const auto val=merge(cap[0].cnt|(static_cast<std::uint32_t>(cap[1].cnt)<<8U));
            for(unsigned i=0;i<2U;++i) {
                const auto next=static_cast<std::uint8_t>(val>>(8U*i));
                if((next&0x80U)!=0U && (cap[i].cnt&0x80U)==0U) {
                    cap[i].written=0U; cap[i].phase=0U;
                }
                cap[i].cnt=next;
            }
        } else if(base==0x04000510U) cap[0].dst=merge(cap[0].dst)&0x07fffffcU;
        else if(base==0x04000514U) cap[0].len=merge(cap[0].len)&0xffffU;
        else if(base==0x04000518U) cap[1].dst=merge(cap[1].dst)&0x07fffffcU;
        else if(base==0x0400051cU) cap[1].len=merge(cap[1].len)&0xffffU;
    }
    std::int32_t output_channel(unsigned n) const {
        const auto& c=ch[n];
        if(!c.busy && !c.holding) return 0;
        const unsigned div=(c.cnt>>8U)&3U;
        return scale7(c.current,c.cnt&127U)>>(div==3U?4U:div);
    }
    void capture(unsigned i,std::int32_t value) {
        auto& c=cap[i];
        if((c.cnt&0x80U)==0U) return;
        const auto limit=std::max(1U,c.len&0xffffU)*4U;
        if(c.written>=limit) {
            if((c.cnt&4U)!=0U) { c.cnt&=static_cast<std::uint8_t>(~0x80U); return; }
            c.written=0U;
        }
        const auto address=c.dst+c.written;
        if((c.cnt&8U)!=0U) {
            const auto q=std::clamp(value>>8U,-128,127);
            bus->write(address,static_cast<std::uint8_t>(static_cast<std::int8_t>(q))); ++c.written;
        } else {
            const auto q=std::clamp(value,-32768,32767);
            const auto v=static_cast<std::uint16_t>(static_cast<std::int16_t>(q));
            for(unsigned b=0;b<2U;++b) bus->write(address+b,static_cast<std::uint8_t>(v>>(8U*b)));
            c.written+=2U;
        }
        if(c.written>=limit && (c.cnt&4U)!=0U) c.cnt&=static_cast<std::uint8_t>(~0x80U);
    }
    void advance_capture(std::uint32_t ticks) {
        for(unsigned i=0;i<cap.size();++i) {
            auto& c=cap[i];
            if((c.cnt&0x80U)==0U) continue;
            c.phase+=ticks;
            const auto p=period(ch[1U+2U*i]);
            while((c.cnt&0x80U)!=0U && c.phase>=p) {
                c.phase-=p;
                std::int32_t value=0;
                if((c.cnt&2U)!=0U) {
                    value=output_channel(2U*i);
                    if((c.cnt&1U)!=0U) value+=output_channel(2U*i+1U);
                } else {
                    for(unsigned n=0;n<ch.size();++n) {
                        const bool bypass=(n==1U && (soundcnt&(1U<<12U))!=0U) ||
                                          (n==3U && (soundcnt&(1U<<13U))!=0U);
                        if(bypass) continue;
                        const auto pan=(ch[n].cnt>>16U)&127U;
                        value+=scale7(output_channel(n),i==0U?127U-pan:pan);
                    }
                }
                capture(i,value);
            }
        }
    }
    void emit() {
        std::int32_t audible_left=0,audible_right=0,ch1_left=0,ch1_right=0,ch3_left=0,ch3_right=0;
        for(unsigned n=0;n<16U;++n) {
            const auto sample=output_channel(n);
            const auto pan=(ch[n].cnt>>16U)&127U;
            const auto left=scale7(sample,127U-pan),right=scale7(sample,pan);
            const bool bypass=(n==1U && (soundcnt&(1U<<12U))!=0U) ||
                              (n==3U && (soundcnt&(1U<<13U))!=0U);
            const bool audible=!debug_muted[n] && (debug_solo<0 || debug_solo==static_cast<int>(n));
            if(audible && !bypass) { audible_left+=left; audible_right+=right; }
            if(n==1U && audible) { ch1_left=left; ch1_right=right; }
            if(n==3U && audible) { ch3_left=left; ch3_right=right; }
        }
        const auto left_source=(soundcnt>>8U)&3U,right_source=(soundcnt>>10U)&3U;
        auto left=audible_left,right=audible_right;
        if(left_source==1U) left=ch1_left;
        else if(left_source==2U) left=ch3_left;
        else if(left_source==3U) left=ch1_left+ch3_left;
        if(right_source==1U) right=ch1_right;
        else if(right_source==2U) right=ch3_right;
        else if(right_source==3U) right=ch1_right+ch3_right;
        if((soundcnt&(1U<<15U))!=0U) {
            left=scale7(left,soundcnt&127U); right=scale7(right,soundcnt&127U);
        } else left=right=0;
        left>>=6U; right>>=6U;
        left=std::clamp(left+static_cast<std::int32_t>(bias),0,1023)-static_cast<std::int32_t>(bias);
        right=std::clamp(right+static_cast<std::int32_t>(bias),0,1023)-static_cast<std::int32_t>(bias);
        if(output_count==output.size()) { output_read=(output_read+2U)%output.size(); output_count-=2U; }
        output[output_write]=static_cast<std::int16_t>(std::clamp(left*64,-32768,32767));
        output_write=(output_write+1U)%output.size();
        output[output_write]=static_cast<std::int16_t>(std::clamp(right*64,-32768,32767));
        output_write=(output_write+1U)%output.size(); output_count+=2U;
    }
    void run(std::int64_t ticks) {
        if(ticks<=0) return;
        auto remain=static_cast<std::uint64_t>(ticks);
        while(remain!=0U) {
            std::uint64_t until=1024U-sample_phase;
            for(unsigned i=0;i<cap.size();++i) {
                const auto& capture=cap[i];
                if((capture.cnt&0x80U)==0U) continue;
                const std::uint64_t p=period(ch[1U+2U*i]);
                until=std::min(until,capture.phase>=p?0U:p-capture.phase);
            }
            const auto step=static_cast<std::uint32_t>(std::min(remain,until));
            for(unsigned n=0;n<ch.size();++n) {
                auto& c=ch[n];
                if(!c.busy) continue;
                c.phase+=step;
                if(c.phase>=period(c)) run_events(c,n);
            }
            advance_capture(step);
            sample_phase+=step; remain-=step;
            if(sample_phase==1024U) { sample_phase=0; emit(); }
        }
    }
    std::size_t drain(std::int16_t* out,std::size_t max_frames) {
        const auto frames=std::min(max_frames,output_count/2U);
        for(std::size_t i=0;i<frames*2U;++i) {
            out[i]=output[output_read]; output_read=(output_read+1U)%output.size();
        }
        output_count-=frames*2U;
        return frames;
    }
};

struct SpuEquivalence {
    TestBus actual_bus,reference_bus;
    dsr::Spu actual;
    ScalarSpu reference;

    SpuEquivalence() {
        reference.bus=&reference_bus;
        for(std::size_t i=0;i<actual_bus.memory.size();++i)
            actual_bus.memory[i]=static_cast<std::uint8_t>((i*73U+(i>>3U)*19U)^0xa5U);
        reference_bus.memory=actual_bus.memory;
        actual.read8=[&](std::uint32_t address) { return actual_bus.read(address); };
        actual.write8=[&](std::uint32_t address,std::uint8_t value) { actual_bus.write(address,value); };
        check();
    }
    void check() {
        assert(actual_bus.events==reference_bus.events);
        assert(actual_bus.memory==reference_bus.memory);
        actual_bus.events.clear(); reference_bus.events.clear();
        for(std::uint32_t a=0x04000400U;a<0x04000520U;++a) {
            assert(actual.io_read(a,8)==reference.io_read(a,8));
            if((a&1U)==0U) assert(actual.io_read(a,16)==reference.io_read(a,16));
            if((a&3U)==0U) assert(actual.io_read(a,32)==reference.io_read(a,32));
        }
        const auto debug=actual.debug_state();
        assert(debug.sound_control==reference.soundcnt && debug.bias==reference.bias);
        assert(debug.solo_channel==reference.debug_solo);
        for(unsigned n=0;n<16U;++n) {
            const auto& c=reference.ch[n];
            const auto& d=debug.channels[n];
            assert(d.format==((c.cnt>>29U)&3U) && d.volume==(c.cnt&127U));
            assert(d.pan==((c.cnt>>16U)&127U) && d.sample_position==c.pos);
            assert(d.current_sample==c.current && d.busy==c.busy && d.muted==reference.debug_muted[n]);
        }
        for(;;) {
            std::array<std::int16_t,128> got{},expected{};
            const auto got_frames=actual.drain(got.data(),got.size()/2U);
            const auto expected_frames=reference.drain(expected.data(),expected.size()/2U);
            assert(got_frames==expected_frames && got==expected);
            if(got_frames==0U) break;
        }
    }
    void reset() { actual.reset(); reference.reset(); check(); }
    void write(std::uint32_t address,std::uint32_t value,int width=32) {
        actual.io_write(address,value,width); reference.io_write(address,value,width); check();
    }
    void run(std::int64_t ticks) { actual.run(ticks); reference.run(ticks); check(); }
    void poke(std::size_t address,std::uint8_t value) {
        actual_bus.memory[address%actual_bus.memory.size()]=value;
        reference_bus.memory[address%reference_bus.memory.size()]=value;
    }
    void mute(unsigned n,bool muted) {
        actual.set_channel_muted(n,muted);
        if(n<16U) reference.debug_muted[n]=muted;
        check();
    }
    void solo(int n) {
        actual.set_solo_channel(n); reference.debug_solo=n>=0 && n<16?n:-1; check();
    }
    void channel(unsigned n,std::uint16_t timer,unsigned fmt=0,unsigned repeat=1,
                 bool hold=false,std::uint32_t len=32,std::uint16_t pnt=0) {
        const auto base=0x04000400U+n*16U;
        write(base+4U,n*128U);
        write(base+8U,timer|(static_cast<std::uint32_t>(pnt)<<16U));
        write(base+12U,len);
        write(base,0x80000000U|(fmt<<29U)|(repeat<<27U)|(hold?1U<<15U:0U)|
                   ((n*29U%128U)<<16U)|127U);
    }
    std::uint32_t position(unsigned n) const { return actual.debug_state().channels[n].sample_position; }
};

void test_channel_deadlines() {
    SpuEquivalence e;
    // Idle time and reset-default timer zero must not become an early deadline.
    e.run(4097);
    e.write(0x0400040cU,256);
    e.write(0x04000400U,0x8800007fU);
    e.run(131071);
    assert(e.position(0)==0U);
    e.run(1);
    assert(e.position(0)==1U);

    e.reset();
    e.channel(0,0xfe00,0,1,false,256); // period 1024
    e.run(777);
    e.write(0x04000408U,0xfffc,16); // period 8, with 777 ticks pending
    assert(e.position(0)==0U); // a write is not an event boundary
    e.run(0);
    e.run(-7);
    assert(e.position(0)==0U);
    e.run(1);
    assert(e.position(0)==97U);
    e.run(5);
    assert(e.position(0)==97U);
    e.run(1);
    assert(e.position(0)==98U);

    e.reset();
    e.channel(0,0xfffc,0,1,false,256);
    e.run(7);
    e.write(0x04000408U,0xfe00,16); // stale period-8 minimum must not fire an event
    e.run(1);
    e.run(1015);
    assert(e.position(0)==0U);
    e.run(1);
    assert(e.position(0)==1U);
    e.write(0x04000400U,0x8800003fU); // Start while busy changes gain, not origin
    e.run(1023);
    assert(e.position(0)==1U);
    e.run(1);
    assert(e.position(0)==2U);

    e.reset();
    e.channel(5,0,0,1,false,256);
    e.run(301);
    e.write(0x04000458U,0xff,8); // low timer byte only
    e.run(1);
    assert(e.position(5)==0U);
    e.write(0x04000459U,0xff,8); // high timer byte makes the pending period 2
    assert(e.position(5)==0U);
    e.run(1);
    assert(e.position(5)==151U);
    e.write(0x0400045aU,3,16); // PNT-only write must retain that partial period
    e.run(1);
    assert(e.position(5)==152U);
}

void test_stop_restart_and_hold() {
    SpuEquivalence e;
    e.write(0x04000500U,0x807f);
    e.channel(0,0xfffe,0,0,true,1); // four PCM8 samples, period 4
    e.run(19);
    assert(e.actual.debug_state().channels[0].busy && e.position(0)==4U);
    e.run(1);
    assert(!e.actual.debug_state().channels[0].busy);
    const auto held=e.actual.debug_state().channels[0].current_sample;
    assert(held!=0);
    e.run(2048);
    e.write(0x04000408U,0xffff,16);
    e.run(11);
    assert(e.actual.debug_state().channels[0].current_sample==held);

    e.write(0x04000400U,0x8000807fU);
    assert(e.position(0)==0U);
    e.run(1);
    assert(e.position(0)==0U);
    e.run(1);
    assert(e.position(0)==1U);
    e.run(1); // stop in the middle of the next period
    e.write(0x04000400U,127);
    assert(!e.actual.debug_state().channels[0].busy);
    assert(e.actual.debug_state().channels[0].current_sample==0);
    e.run(47);
    e.write(0x04000400U,0x8800007fU);
    e.run(1);
    assert(e.position(0)==0U);
    e.run(1);
    assert(e.position(0)==1U);

    // Reset with an active channel/capture must discard both cached deadlines
    // and accumulated origins, while the caller's memory callbacks survive.
    e.write(0x04000418U,0xffff,16);
    e.write(0x04000508U,0x8a,8);
    e.run(1);
    e.reset();
    e.run(4097);
    e.channel(14,0xffff,3);
    e.run(1);
    assert(e.actual.debug_state().channels[14].current_sample==0);
    e.run(1);
    assert(e.actual.debug_state().channels[14].current_sample==-32768);
}

void test_channel_and_capture_order() {
    SpuEquivalence e;
    e.write(0x04000500U,0x807f);
    for(unsigned n=0;n<16U;++n) {
        const unsigned fmt=n<12U?n%4U:3U;
        e.channel(n,static_cast<std::uint16_t>(0xffffU-n%4U),fmt,1,n%2U!=0U,8,
                  static_cast<std::uint16_t>(n%3U));
    }
    // Multiple overdue events on each channel still execute channel-major,
    // rather than being interleaved in nominal timer-deadline order.
    e.run(127);
    e.run(1);
    e.run(2049);

    // Both captures feed back into sample buffers. Read/write ordering is
    // observable even when the eventual DAC samples happen to match.
    e.write(0x04000418U,0xfff8,16);
    e.write(0x04000438U,0xfffb,16);
    e.write(0x04000510U,128);
    e.write(0x04000514U,8);
    e.write(0x04000518U,0);
    e.write(0x0400051cU,8);
    e.write(0x04000508U,0x808b,16); // direct PCM8 sum, mixed PCM16
    e.run(4097);
    e.write(0x04000418U,0xffff,16);
    e.write(0x04000438U,0xff00,16);
    e.run(1);
    e.run(2049);
}

void test_capture_deadlines() {
    SpuEquivalence e;
    for(unsigned unit=0;unit<2U;++unit) {
        e.reset();
        const unsigned source=2U*unit,clock_channel=1U+source;
        const auto clock=0x04000408U+clock_channel*16U;
        const auto destination=0x04000510U+unit*8U;
        const auto control=0x04000508U+unit;
        e.channel(source,0xff00); // source period 512, capture period 1024
        e.write(clock,0xfe00,16); // capture clock playback remains stopped
        e.write(destination,2048U+unit*32U);
        e.write(destination+4U,8);
        e.write(control,0x8b,8);
        e.run(777);
        e.write(0x04000408U+source*16U,0xfffc,16);
        e.write(clock,0xfffc,16);
        e.run(0);
        // Capture is already overdue: the next run first has a zero-tick
        // boundary. Playback catches up there before either capture unit.
        e.run(1);
        assert(e.position(source)==34U);
        assert(e.reference.cap[unit].written==1U); // 97 PCM8 captures, wrapping at 32
        e.write(0x04000400U+clock_channel*16U,0xe800007fU);
        e.write(clock,0xff00,16); // increasing period retains capture's phase
        e.run(509);
        assert(e.reference.cap[unit].written==1U);
        e.run(1);
        assert(e.reference.cap[unit].written==2U);
        e.write(0x04000400U+clock_channel*16U,0);
        e.run(512); // capture continues after its clock's playback stops
        assert(e.reference.cap[unit].written==3U);
        e.write(control,0,8);
        e.run(777);
        assert(e.reference.cap[unit].written==3U);
        e.write(destination+4U,0);
        e.write(control,0x86,8); // zero-length one-shot PCM16 means one word
        e.run(1024);
        assert((e.actual.io_read(control,8)&0x80U)==0U);
        e.write(destination+4U,8);
        e.write(control,0x8a,8);
        e.run(123);
        e.write(control,0x8b,8); // already-enabled mode write does not reset phase
        e.run(389);
        assert(e.reference.cap[unit].written==1U);
    }
}

void test_zero_sample_mixing() {
    SpuEquivalence e;
    for(std::size_t i=0;i<e.actual_bus.memory.size();++i) e.poke(i,0);
    for(std::size_t i=0;i<128U;++i) e.poke(i,64);
    e.channel(0,0xfe00,0,1,false,1);
    e.channel(1,0xfe00,0,1,false,1);
    e.channel(3,0xfe00,0,1,false,1);
    e.channel(5,0xfe00,0,1,false,1);
    e.poke(640,127);
    e.write(0x04000450U,0,8); // nonzero raw sample, zero channel gain
    for(unsigned left=0;left<4U;++left) {
        for(unsigned right=0;right<4U;++right) {
            for(unsigned bypass=0;bypass<4U;++bypass) {
                e.write(0x04000500U,0x8000U|((left+right)%2U==0U?127U:0U)|
                        (left<<8U)|(right<<10U)|(bypass<<12U));
                e.write(0x04000504U,(left+right+bypass)%3U==0U?0U:
                        ((left+right+bypass)%3U==1U?512U:1023U));
                e.mute(1,(bypass&1U)!=0U);
                e.mute(3,(bypass&2U)!=0U);
                e.solo(bypass==0U?-1:(bypass==1U?1:(bypass==2U?3:0)));
                e.run(1024);
            }
        }
    }
}

std::uint32_t random_word(std::uint32_t& state) {
    state^=state<<13U; state^=state>>17U; state^=state<<5U;
    return state;
}

void test_random_scalar_equivalence(std::uint32_t seed,bool split_intervals) {
    SpuEquivalence e;
    std::uint32_t split_seed=seed^0x9e3779b9U;
    constexpr std::array<std::uint16_t,8> timers={0,0x8000,0xfe00,0xff00,0xff80,0xfffc,0xfffe,0xffff};
    const auto next=[&]() { return random_word(seed); };
    const auto advance=[&](std::uint32_t ticks) {
        if(!split_intervals) { e.run(ticks); return; }
        // Compare each partition against the old scalar at identical call
        // boundaries; repartitioning itself need not preserve SPU behavior.
        e.run(0);
        while(ticks!=0U) {
            const auto step=std::min(ticks,1U+random_word(split_seed)%128U);
            e.run(step); ticks-=step;
        }
    };
    e.write(0x04000500U,0x807f);
    for(unsigned n=0;n<16U;++n)
        e.channel(n,timers[n%timers.size()],n<12U?n%4U:3U,1,n%2U!=0U,8,
                  static_cast<std::uint16_t>(n%4U));
    for(unsigned iteration=0;iteration<512U;++iteration) {
        const unsigned n=next()%16U;
        const auto base=0x04000400U+n*16U;
        const auto timer=timers[next()%timers.size()];
        switch(next()%12U) {
        case 0: advance(next()%129U); break;
        case 1: advance(next()%4097U); break;
        case 2:
            switch(next()%4U) {
            case 0: e.write(base+8U,timer&255U,8); break;
            case 1: e.write(base+9U,timer>>8U,8); break;
            case 2: e.write(base+8U,timer,16); break;
            default: {
                const auto pnt=next()%9U;
                e.write(base+8U,timer|(pnt<<16U));
                break;
            }
            }
            break;
        case 3: e.write(base,next()); break; // includes Start set while busy and explicit stops
        case 4: {
            const unsigned fmt=next()%4U,repeat=next()%4U;
            const bool hold=(next()&1U)!=0U;
            const auto len=next()%33U;
            const auto pnt=static_cast<std::uint16_t>(next()%9U);
            e.write(base,0);
            e.channel(n,timer,fmt,repeat,hold,len,pnt);
            break;
        }
        case 5:
            for(unsigned i=0;i<8U;++i) {
                const auto address=next()%e.actual_bus.memory.size();
                e.poke(address,static_cast<std::uint8_t>(next()));
            }
            e.check();
            break;
        case 6: {
            const unsigned unit=next()%2U;
            const auto address=next()%2048U;
            e.write(0x04000510U+unit*8U,address);
            e.write(0x04000514U+unit*8U,next()%9U);
            e.write(0x04000508U+unit,0x80U|(next()&15U),8);
            e.write(0x04000418U+unit*32U,timer,16);
            break;
        }
        case 7: {
            const auto first=next()&0x8fU,second=next()&0x8fU;
            e.write(0x04000508U,first|(second<<8U),16);
            break;
        }
        case 8:
            e.write(0x04000500U,next(),16);
            e.write(0x04000504U,next(),16);
            break;
        case 9: {
            const unsigned byte=next()%4U;
            e.write(base+byte,next(),8);
            e.write(base+4U,next()%2048U);
            break;
        }
        case 10: {
            const unsigned channel=next()%18U;
            e.mute(channel,(next()&1U)!=0U);
            e.solo(static_cast<int>(next()%19U)-2);
            break;
        }
        default:
            if((next()&3U)==0U) e.reset();
            else advance(next()%1025U);
            break;
        }
    }
    advance(8193);
}

void test_scalar_equivalence() {
    test_channel_deadlines();
    test_stop_restart_and_hold();
    test_channel_and_capture_order();
    test_capture_deadlines();
    test_zero_sample_mixing();
    for(const auto seed:{0x12345678U,0xdeadbeefU,0x31415926U,0x6d2b79f5U}) {
        test_random_scalar_equivalence(seed,false);
        test_random_scalar_equivalence(seed,true);
    }
}

} // namespace

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

    test_scalar_equivalence();

    return 0;
}
