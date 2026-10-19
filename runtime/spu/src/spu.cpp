#include "dsr/spu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace dsr {
namespace {
constexpr std::size_t kRingSamples=65536;

constexpr std::array<int, 89> kStepTable = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
constexpr std::array<int, 8> kIndexAdjust = {-1,-1,-1,-1,2,4,6,8};
constexpr std::array<std::array<int, 8>, 8> kDuty = {{
    {{0,0,0,0,0,0,0,1}},
    {{0,0,0,0,0,0,1,1}},
    {{0,0,0,0,0,1,1,1}},
    {{0,0,0,0,1,1,1,1}},
    {{0,0,0,1,1,1,1,1}},
    {{0,0,1,1,1,1,1,1}},
    {{0,1,1,1,1,1,1,1}},
    {{0,0,0,0,0,0,0,0}},
}};
}

struct Spu::Impl {
    struct Channel {
        std::uint32_t cnt=0, sad=0, len=0;
        std::uint16_t timer=0, pnt=0;
        bool busy=false, holding=false;
        std::uint64_t phase=0;
        std::uint32_t pos=0;
        std::int32_t current=0, predictor=0, initial_predictor=0;
        int adpcm_index=0, initial_adpcm_index=0;
        std::uint32_t lfsr=0x7fff;
        unsigned duty_pos=0;
        std::uint32_t loop_pos=0;
        std::int32_t loop_predictor=0;
        int loop_index=0;
        bool loop_state_valid=false;
    };
    struct Capture {
        std::uint8_t cnt=0;
        std::uint32_t dst=0, len=0, written=0;
        std::uint64_t phase=0;
    };
    std::array<Channel,16> ch{};
    std::array<bool,16> debug_muted{};
    int debug_solo=-1;
    std::array<Capture,2> cap{};
    std::uint16_t soundcnt=0, bias=0x0200;
    std::uint64_t sample_phase=0;
    std::array<std::int16_t,kRingSamples> output{};
    std::size_t output_read=0, output_write=0, output_count=0;
    std::int32_t last_left=0, last_right=0;
    bool linear=false;

    void stop(Channel& c) { c.busy=false; c.holding=false; c.current=0; }
    void finish(Channel& c) {
        c.busy=false;
        c.holding=(c.cnt&(1U<<15U))!=0U;
        if(!c.holding) c.current=0;
    }
    std::uint32_t period(const Channel& c, unsigned n) const {
        (void)n;
        return std::max(1U,static_cast<std::uint32_t>(0x10000U-c.timer)*2U);
    }
    std::uint32_t sample_count(const Channel& c) const {
        const unsigned fmt=(c.cnt>>29U)&3U;
        const std::uint32_t words=static_cast<std::uint32_t>(c.pnt)+c.len;
        if(fmt==0U) return words*4U;
        if(fmt==1U) return words*2U;
        if(fmt==2U) return words*8U;
        return 0;
    }
    static std::int32_t scale7(std::int32_t value, unsigned multiplier) {
        return multiplier==127U ? value : (value*static_cast<std::int32_t>(multiplier))>>7U;
    }
    void start(Channel& c) {
        c.busy=true; c.holding=false; c.phase=0; c.pos=0; c.current=0; c.duty_pos=0;
        c.predictor=0; c.adpcm_index=0; c.loop_state_valid=false;
        c.lfsr=0x7fff;
        const unsigned fmt=(c.cnt>>29U)&3U;
        if(fmt==2U) {
            const std::uint32_t h=rd(c.sad)&255U;
            const std::uint32_t h1=rd(c.sad+1U)&255U;
            c.predictor=static_cast<std::int16_t>(h|(h1<<8U));
            c.current=c.predictor;
            c.adpcm_index=std::min(88,static_cast<int>(rd(c.sad+2U)&127U));
            c.initial_predictor=c.predictor;
            c.initial_adpcm_index=c.adpcm_index;
            // ADPCM sample position zero is the 32-bit header; the first
            // decoded nibble is position eight (SAD + 4).
            c.pos=8U;
            if(c.pnt==0U) {
                c.loop_pos=0U;
                c.loop_predictor=c.predictor;
                c.loop_index=c.adpcm_index;
                c.loop_state_valid=true;
            }
        }
    }
    // The bus callbacks live on the owning Spu; keeping a pointer avoids copying two
    // std::function objects on every run()/io call.
    const Spu* owner=nullptr;
    std::uint32_t rd(std::uint32_t addr) const { return owner!=nullptr && owner->read8 ? owner->read8(addr) : 0U; }
    bool can_write() const { return owner!=nullptr && static_cast<bool>(owner->write8); }
    void wr(std::uint32_t addr,std::uint8_t value) const { owner->write8(addr,value); }
    void decode_adpcm(Channel& c) {
        const std::uint32_t total=sample_count(c);
        if(c.pos>=total) {
            const unsigned repeat=(c.cnt>>27U)&3U;
            if(repeat!=1U) { finish(c); return; }
            const std::uint32_t loop_start=static_cast<std::uint32_t>(c.pnt)*8U;
            if(loop_start==0U) {
                c.pos=8U;
                c.predictor=c.initial_predictor;
                c.adpcm_index=c.initial_adpcm_index;
            } else {
                c.pos=loop_start;
                if(c.loop_state_valid && c.loop_pos==loop_start) {
                    c.predictor=c.loop_predictor;
                    c.adpcm_index=c.loop_index;
                }
            }
        }
        if(c.pos>=total) { finish(c); return; }
        const std::uint32_t nibble=c.pos;
        const std::uint32_t loop_start=static_cast<std::uint32_t>(c.pnt)*8U;
        if(!c.loop_state_valid && nibble==loop_start) {
            c.loop_pos=nibble; c.loop_predictor=c.predictor; c.loop_index=c.adpcm_index; c.loop_state_valid=true;
        }
        const std::uint32_t raw=rd(c.sad+nibble/2U)&255U;
        const int code=static_cast<int>((nibble&1U)?(raw>>4U):(raw&15U));
        const int step=kStepTable[static_cast<std::size_t>(c.adpcm_index)];
        const int diff=((code&7)*2+1)*step>>3;
        c.predictor=std::clamp(c.predictor+((code&8)!=0?-diff:diff),-32768,32767);
        c.adpcm_index=std::clamp(c.adpcm_index+kIndexAdjust[static_cast<std::size_t>(code&7)],0,88);
        c.current=c.predictor; ++c.pos;
    }
    void advance(Channel& c,unsigned n,std::uint32_t ticks) {
        if(!c.busy) return;
        const std::uint64_t p=period(c,n);
        c.phase+=ticks;
        while(c.busy && c.phase>=p) {
            c.phase-=p;
            const unsigned fmt=(c.cnt>>29U)&3U;
            if(fmt==2U) { decode_adpcm(c); continue; }
            if(fmt==0U || fmt==1U) {
                const std::uint32_t total=sample_count(c);
                if(c.pos>=total) {
                    if(((c.cnt>>27U)&3U)!=1U) { finish(c); break; }
                    const std::uint32_t loop_start=static_cast<std::uint32_t>(c.pnt)*(fmt==0U?4U:2U);
                    if(loop_start>=total) { finish(c); break; }
                    c.pos=loop_start;
                }
                if(fmt==0U) {
                    const auto b=rd(c.sad+c.pos)&255U;
                    c.current=static_cast<std::int8_t>(b)*256; ++c.pos;
                } else {
                    const auto lo=rd(c.sad+c.pos*2U)&255U;
                    const auto hi=rd(c.sad+c.pos*2U+1U)&255U;
                    c.current=static_cast<std::int16_t>(lo|(hi<<8U)); ++c.pos;
                }
            } else if(n<8U) {
                c.current=0;
            } else if(n<14U) {
                ++c.duty_pos; c.duty_pos&=7U;
                const unsigned duty=(c.cnt>>24U)&7U;
                c.current=kDuty[duty][c.duty_pos]!=0?32767:-32768;
            } else {
                c.current=(c.lfsr&1U)!=0U?-32768:32767;
                if((c.lfsr&1U)!=0U) c.lfsr=(c.lfsr>>1U)^0x6000U;
                else c.lfsr>>=1U;
            }
        }
    }
    std::uint32_t io_read(std::uint32_t addr,int width) {
        const std::uint32_t base=addr&~3U; std::uint32_t v=0;
        if(base>=0x04000400U && base<0x04000500U) {
            const unsigned n=(base-0x04000400U)/16U, off=(base-0x04000400U)%16U;
            const Channel& c=ch[n];
            if(off==0U) v=(c.cnt&0x7fffffffU)|(c.busy?0x80000000U:0U);
            else if(off==4U) v=c.sad;
            else if(off==8U) v=c.timer|(static_cast<std::uint32_t>(c.pnt)<<16U);
            else if(off==12U) v=c.len;
        } else if(base==0x04000500U) v=soundcnt;
        else if(base==0x04000504U) v=bias;
        else if(base==0x04000508U) v=static_cast<std::uint32_t>(cap[0].cnt)|(static_cast<std::uint32_t>(cap[1].cnt)<<8U);
        else if(base==0x04000510U) v=cap[0].dst;
        else if(base==0x04000514U) v=cap[0].len;
        else if(base==0x04000518U) v=cap[1].dst;
        else if(base==0x0400051cU) v=cap[1].len;
        const unsigned shift=(addr&3U)*8U;
        if(width==1) return (v>>shift)&255U;
        if(width==2) return (v>>shift)&65535U;
        return v;
    }
    void io_write(std::uint32_t addr,std::uint32_t value,int width) {
        const std::uint32_t base=addr&~3U; const unsigned shift=(addr&3U)*8U;
        const std::uint32_t mask=width==1?255U:(width==2?65535U:0xffffffffU);
        const std::uint32_t placed=width==4?value:(value&mask)<<shift;
        const std::uint32_t pmask=width==4?mask:mask<<shift;
        if(base>=0x04000400U && base<0x04000500U) {
            const unsigned n=(base-0x04000400U)/16U, off=(base-0x04000400U)%16U; Channel& c=ch[n];
            if(off==0U) {
                const bool was_busy=c.busy;
                c.cnt=(c.cnt&~pmask)|(placed&pmask);
                if((placed&pmask&0x80000000U)!=0U) {
                    if(!was_busy) start(c);
                } else if((pmask&0x80000000U)!=0U && c.busy) stop(c);
                c.cnt&=0x7fffffffU;
            } else if(off==4U) c.sad=((c.sad&~pmask)|(placed&pmask))&0x07fffffcU;
            else if(off==8U) {
                c.timer=static_cast<std::uint16_t>((c.timer&~(pmask&65535U))|(placed&pmask&65535U));
                c.pnt=static_cast<std::uint16_t>((c.pnt&~((pmask>>16U)&65535U))|((placed>>16U)&((pmask>>16U)&65535U)));
            } else if(off==12U) c.len=((c.len&~pmask)|(placed&pmask))&0x003fffffU;
            return;
        }
        auto merge=[&](std::uint32_t old) { return (old&~pmask)|(placed&pmask); };
        if(base==0x04000500U) soundcnt=static_cast<std::uint16_t>(merge(soundcnt)&0xbf7fU);
        else if(base==0x04000504U) bias=static_cast<std::uint16_t>(merge(bias)&0x03ffU);
        else if(base==0x04000508U) {
            const std::uint32_t old=static_cast<std::uint32_t>(cap[0].cnt)|(static_cast<std::uint32_t>(cap[1].cnt)<<8U);
            const std::uint32_t val=merge(old);
            for(unsigned i=0;i<2;++i) {
                const auto next=static_cast<std::uint8_t>(val>>(8U*i));
                if((next&0x80U)!=0U && (cap[i].cnt&0x80U)==0U) {
                    cap[i].written=0U;
                    cap[i].phase=0U;
                }
                cap[i].cnt=next;
            }
        } else if(base==0x04000510U) cap[0].dst=merge(cap[0].dst)&0x07fffffcU;
        else if(base==0x04000514U) cap[0].len=merge(cap[0].len)&0xffffU;
        else if(base==0x04000518U) cap[1].dst=merge(cap[1].dst)&0x07fffffcU;
        else if(base==0x0400051cU) cap[1].len=merge(cap[1].len)&0xffffU;
    }
    std::int32_t output_channel(unsigned n) const {
        const Channel& c=ch[n];
        if(!c.busy && !c.holding) return 0;
        const unsigned div=(c.cnt>>8U)&3U;
        const unsigned shift=div==3U?4U:div;
        return scale7(c.current,c.cnt&127U)>>shift;
    }
    void capture(unsigned i,std::int32_t value) {
        Capture& c=cap[i];
        if((c.cnt&0x80U)==0U || !can_write()) return;
        const bool is8=(c.cnt&8U)!=0U;
        const std::uint32_t limit=std::max(1U,c.len&0xffffU)*4U;
        if(c.written>=limit) {
            if((c.cnt&4U)!=0U) { c.cnt&=static_cast<std::uint8_t>(~0x80U); return; }
            c.written=0U;
        }
        const std::uint32_t address=c.dst+c.written;
        if(is8) {
            const std::int32_t q=std::clamp(value>>8U,-128,127);
            wr(address,static_cast<std::uint8_t>(static_cast<std::int8_t>(q)));
            ++c.written;
        } else {
            const std::int32_t q=std::clamp(value,-32768,32767);
            const auto v=static_cast<std::uint16_t>(static_cast<std::int16_t>(q));
            for(unsigned b=0;b<2U;++b)
                wr(address+b,static_cast<std::uint8_t>(v>>(8U*b)));
            c.written+=2U;
        }
        if(c.written>=limit && (c.cnt&4U)!=0U) c.cnt&=static_cast<std::uint8_t>(~0x80U);
    }
    void advance_capture(std::uint32_t ticks) {
        for(unsigned i=0;i<cap.size();++i) {
            Capture& c=cap[i];
            if((c.cnt&0x80U)==0U) continue;
            c.phase+=ticks;
            const std::uint32_t p=period(ch[1U+2U*i],1U+2U*i);
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
                        const unsigned pan=(ch[n].cnt>>16U)&127U;
                        value+=scale7(output_channel(n),i==0U?127U-pan:pan);
                    }
                }
                capture(i,value);
            }
        }
    }
    void emit() {
        std::int32_t audible_left=0,audible_right=0;
        std::int32_t audible_ch1_left=0,audible_ch1_right=0,audible_ch3_left=0,audible_ch3_right=0;
        for(unsigned n=0;n<16U;++n) {
            const std::int32_t sample=output_channel(n);
            const unsigned pan=(ch[n].cnt>>16U)&127U;
            const std::int32_t left=scale7(sample,127U-pan);
            const std::int32_t right=scale7(sample,pan);
            const bool bypass=(n==1U && (soundcnt&(1U<<12U))!=0U) ||
                              (n==3U && (soundcnt&(1U<<13U))!=0U);
            const bool audible=!debug_muted[n] && (debug_solo<0 || debug_solo==static_cast<int>(n));
            if(audible && !bypass) { audible_left+=left; audible_right+=right; }
            if(n==1U && audible) { audible_ch1_left=left; audible_ch1_right=right; }
            if(n==3U && audible) { audible_ch3_left=left; audible_ch3_right=right; }
        }
        const unsigned left_source=(soundcnt>>8U)&3U, right_source=(soundcnt>>10U)&3U;
        std::int32_t left=audible_left,right=audible_right;
        if(left_source==1U) left=audible_ch1_left;
        else if(left_source==2U) left=audible_ch3_left;
        else if(left_source==3U) left=audible_ch1_left+audible_ch3_left;
        if(right_source==1U) right=audible_ch1_right;
        else if(right_source==2U) right=audible_ch3_right;
        else if(right_source==3U) right=audible_ch1_right+audible_ch3_right;
        if((soundcnt&(1U<<15U))!=0U) {
            const unsigned master_vol=soundcnt&127U;
            left=scale7(left,master_vol); right=scale7(right,master_vol);
        } else left=right=0;
        // The mixer feeds a 10-bit DAC, while decoded channel samples are s16.
        left>>=6U; right>>=6U;
        left=std::clamp(left+static_cast<std::int32_t>(bias),0,1023)-static_cast<std::int32_t>(bias);
        right=std::clamp(right+static_cast<std::int32_t>(bias),0,1023)-static_cast<std::int32_t>(bias);
        const int l=left*64, r=right*64;
        const int outl=linear?(l+last_left)/2:l, outr=linear?(r+last_right)/2:r;
        last_left=l; last_right=r;
        if(output_count==output.size()) { output_read=(output_read+2U)%output.size(); output_count-=2U; }
        output[output_write]=static_cast<std::int16_t>(std::clamp(outl,-32768,32767));
        output_write=(output_write+1U)%output.size();
        output[output_write]=static_cast<std::int16_t>(std::clamp(outr,-32768,32767));
        output_write=(output_write+1U)%output.size();
        output_count+=2U;
    }
};

Spu::Spu() : impl_(new Impl) { reset(); }
Spu::~Spu() { delete impl_; }
void Spu::reset() { *impl_=Impl{}; impl_->owner=this; }
// Public widths are in bits (8/16/32) like the core bus; Impl works in bytes.
std::uint32_t Spu::io_read(std::uint32_t a,int w) { impl_->owner=this; return impl_->io_read(a,w/8); }
void Spu::io_write(std::uint32_t a,std::uint32_t v,int w) { impl_->owner=this; impl_->io_write(a,v,w/8); }
void Spu::run(std::int64_t ticks) {
    impl_->owner=this;
    if(ticks<=0) return;
    auto remain=static_cast<std::uint64_t>(ticks);
    while(remain!=0U) {
        std::uint64_t until=static_cast<std::uint64_t>(kTicksPerSample)-impl_->sample_phase;
        // Capture uses the channel 1/3 timer even if that playback channel is
        // stopped. Split at those overflows, not just at host output samples,
        // so captures observe the source waveform at their own sample rate.
        for(unsigned i=0;i<impl_->cap.size();++i) {
            const auto& capture=impl_->cap[i];
            if((capture.cnt&0x80U)==0U) continue;
            const std::uint64_t p=impl_->period(impl_->ch[1U+2U*i],1U+2U*i);
            until=std::min(until,capture.phase>=p?0U:p-capture.phase);
        }
        const std::uint32_t step=static_cast<std::uint32_t>(std::min(remain,until));
        for(unsigned n=0;n<16;++n) impl_->advance(impl_->ch[n],n,step);
        impl_->advance_capture(step);
        impl_->sample_phase+=step; remain-=step;
        if(impl_->sample_phase==static_cast<std::uint64_t>(kTicksPerSample)) { impl_->sample_phase=0; impl_->emit(); }
    }
}
std::size_t Spu::drain(std::int16_t* out,std::size_t max_frames) {
    if(out==nullptr) return 0;
    const std::size_t frames=std::min(max_frames,impl_->output_count/2U);
    for(std::size_t i=0;i<frames*2U;++i) {
        out[i]=impl_->output[impl_->output_read];
        impl_->output_read=(impl_->output_read+1U)%impl_->output.size();
    }
    impl_->output_count-=frames*2U;
    return frames;
}

Spu::DebugState Spu::debug_state() const {
    DebugState state;
    state.sound_control = impl_->soundcnt;
    state.bias = impl_->bias;
    state.solo_channel = impl_->debug_solo;
    for (std::size_t i = 0; i < state.channels.size(); ++i) {
        const auto& source = impl_->ch[i];
        auto& channel = state.channels[i];
        channel.format = (source.cnt >> 29U) & 3U;
        channel.volume = source.cnt & 0x7fU;
        channel.pan = (source.cnt >> 16U) & 0x7fU;
        channel.sample_position = source.pos;
        channel.current_sample = source.current;
        channel.busy = source.busy;
        channel.muted = impl_->debug_muted[i];
    }
    return state;
}
void Spu::set_channel_muted(unsigned channel, bool muted) {
    if (channel < impl_->debug_muted.size()) impl_->debug_muted[channel] = muted;
}
void Spu::set_solo_channel(int channel) {
    impl_->debug_solo = channel >= 0 && channel < 16 ? channel : -1;
}

} // namespace dsr
