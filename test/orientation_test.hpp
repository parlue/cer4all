#pragma once
#include "orientation.hpp"
#include <cassert>
#include <string>
#include <vector>
#include <random>
namespace orientation_test {
using namespace cer4all;
using Bytes = std::vector<uint8_t>;
inline void collect(const uint8_t *p, size_t n, void *ctx) {
    auto &v = *static_cast<Bytes *>(ctx); v.insert(v.end(),p,p+n);
}
inline PhysicalBoard startBoard() {
    PhysicalBoard b{};
    for (unsigned i=0;i<64;++i) if(i<16||i>=48)
        b[i] = {173,99,uint8_t(i+1),12,77}; // arbitrary IDs, no vendor prefixes
    return b;
}
inline PhysicalBoard reverse(const PhysicalBoard &b) {
    PhysicalBoard out{};
    for(unsigned i=0;i<64;++i)out[i]=b[63-i];
    return out;
}
inline std::string line(const PhysicalBoard &b) {
    std::string s=":";
    for(auto tag:b)for(auto v:tag)s+=std::to_string(v)+" ";
    return s+"\r\n";
}
inline void feed(OrientedPositionPipe &p,const std::string &s,size_t chunk,Bytes &out) {
    for(size_t i=0;i<s.size();i+=chunk)
        p.feed(reinterpret_cast<const uint8_t *>(s.data()+i),std::min(chunk,s.size()-i),collect,&out);
}
inline void run() {
    auto normal=startBoard(), turned=reverse(normal);
    // Compare the new normal path directly with the previous forwarding pipe,
    // including malformed data, acknowledgements, Sentio and partial records.
    PhysicalBoard empty{};
    const auto corpus=std::string("L D\r\n:999 1 L2 bad\r\n")+line(empty)+
        ":255 255 0 0 0 0 255 255\nD:173 99 255 0 12 ";
    for(auto type:{Model::Unknown,Model::Certabo,Model::Spectrum,Model::Sentio})
        for(size_t chunk=1;chunk<=253;++chunk) {
            PositionPipe previous;previous.reset(type);
            OrientedPositionPipe current;current.reset(type);
            Bytes a,c;
            for(size_t i=0;i<corpus.size();i+=chunk) {
                size_t n=std::min(chunk,corpus.size()-i);
                auto bytes=reinterpret_cast<const uint8_t *>(corpus.data()+i);
                previous.feed(bytes,n,collect,&a);current.feed(bytes,n,collect,&c);
                assert(a==c&&current.type()==previous.type()&&current.frames()==previous.frames());
            }
        }
    BoardOrientation b;
    for(int i=0;i<3;++i)assert(!b.observe(normal));
    assert(b.learned()&&!b.rotated());
    auto missing=turned;missing[0]={};
    for(int i=0;i<10;++i)assert(!b.observe(missing));
    auto duplicate=turned;duplicate[0]=duplicate[1];
    for(int i=0;i<10;++i)assert(!b.observe(duplicate));
    assert(!b.observe(turned));assert(!b.observe(turned));b.invalid();
    assert(!b.observe(turned));assert(!b.observe(turned));assert(b.observe(turned));
    assert(b.rotated());
    for(int i=0;i<10;++i)assert(!b.observe(turned));
    // Same-colour same-type pieces may exchange places. Spare queens irrelevant.
    auto swapped=normal;std::swap(swapped[8],swapped[9]);std::swap(swapped[0],swapped[7]);
    swapped[19]={7,22,31,44,55};swapped[43]={8,21,30,43,54};
    assert(!b.observe(swapped));assert(!b.observe(swapped));assert(b.observe(swapped));
    assert(!b.rotated());
    for(size_t chunk=1;chunk<=253;++chunk) {
        OrientedPositionPipe p;p.reset(Model::Spectrum);Bytes out;
        // Learning and normal play must be immediate and byte-identical.
        auto stream=std::string("D\r\n")+line(normal)+line(normal)+line(normal)+":999 L bad\n";
        for(size_t i=0;i<stream.size();i+=chunk) {
            size_t n=std::min(chunk,stream.size()-i);
            p.feed(reinterpret_cast<const uint8_t *>(stream.data()+i),n,collect,&out);
            assert(std::string(out.begin(),out.end())==stream.substr(0,i+n));
        }
        assert(p.learned()&&!p.rotated());
        for(int i=0;i<3;++i)feed(p,line(turned),chunk,out);
        assert(p.rotated());out.clear();
        auto position=turned;position[0]={};position[27]={9,88,77,66,55};
        auto raw=line(position);
        for(size_t i=0;i<raw.size()-1;++i) {
            p.feed(reinterpret_cast<const uint8_t *>(raw.data()+i),1,collect,&out);
            assert(out.empty());
        }
        p.feed(reinterpret_cast<const uint8_t *>(raw.data()+raw.size()-1),1,collect,&out);
        assert(std::string(out.begin(),out.end())==line(reverse(position)));
        // Invalid records are not partially reordered, acknowledgements survive.
        out.clear();feed(p,"D:999 L broken\r\n",chunk,out);
        assert(std::string(out.begin(),out.end())=="D:999 L broken\r\n");
        out.clear();feed(p,"D"+line(turned),chunk,out);
        assert(std::string(out.begin(),out.end())=="D"+line(normal));
        for(int i=0;i<3;++i)feed(p,line(normal),chunk,out);
        assert(!p.rotated());out.clear();
        feed(p,raw,chunk,out);assert(std::string(out.begin(),out.end())==raw);
        p.reset(Model::Spectrum);assert(!p.learned()&&!p.rotated());
    }
    // A truncated frame breaks a candidate sequence. Transition emits a full
    // normalized position immediately in the new coordinate epoch.
    OrientedPositionPipe interrupted;interrupted.reset(Model::Spectrum);Bytes interruptedOut;
    for(int i=0;i<3;++i)feed(interrupted,line(normal),64,interruptedOut);
    for(int i=0;i<2;++i)feed(interrupted,line(turned),64,interruptedOut);
    feed(interrupted,":123 44 ",1,interruptedOut);
    feed(interrupted,line(turned),64,interruptedOut);assert(!interrupted.rotated());
    feed(interrupted,line(turned),64,interruptedOut);assert(!interrupted.rotated());
    interruptedOut.clear();feed(interrupted,line(turned),64,interruptedOut);
    assert(interrupted.rotated());
    auto normalized=line(normal);
    assert(std::string(interruptedOut.end()-normalized.size(),interruptedOut.end())==normalized);
    struct Capture { Bytes bytes; std::vector<bool> turns; } capture;
    auto sink=[](const uint8_t *p,size_t n,void *ctx) {auto &v=static_cast<Capture *>(ctx)->bytes;v.insert(v.end(),p,p+n);};
    auto boundary=[](bool turn,void *ctx) {auto &v=*static_cast<Capture *>(ctx);v.turns.push_back(turn);v.bytes.clear();};
    OrientedPositionPipe batched;batched.reset(Model::Certabo);
    const auto batch=line(normal)+line(normal)+line(normal)+line(turned)+line(turned)+line(turned)+line(normal)+line(normal)+line(normal);
    batched.feed(reinterpret_cast<const uint8_t *>(batch.data()),batch.size(),sink,&capture,boundary);
    assert(capture.turns==std::vector<bool>({true,false}));
    assert(std::string(capture.bytes.begin(),capture.bytes.end())==line(normal));
    // Rotation must preserve arbitrary IDs, duplicates, noise and empty fields;
    // detection never turns into a piece filter.
    OrientedPositionPipe noisy;noisy.reset(Model::Spectrum);Bytes noiseOut;
    for(int i=0;i<3;++i)feed(noisy,line(normal),64,noiseOut);
    for(int i=0;i<3;++i)feed(noisy,line(turned),64,noiseOut);
    std::mt19937 rng(301026);
    for(unsigned trial=0;trial<300;++trial) {
        PhysicalBoard values{};
        for(auto &tag:values) for(auto &v:tag)v=uint8_t(rng());
        values[1]=values[0];values[12]={};values[43]=normal[59];
        noiseOut.clear();feed(noisy,line(values),trial%253+1,noiseOut);
        assert(noisy.rotated());
        assert(std::string(noiseOut.begin(),noiseOut.end())==line(reverse(values)));
    }
    // Unknown boards and Sentio must not learn an orientation from colourless data.
    for(auto type:{Model::Unknown,Model::Sentio}) {
        OrientedPositionPipe p;p.reset(type);Bytes out;
        for(int i=0;i<4;++i)feed(p,line(normal),64,out);
        for(int i=0;i<4;++i)feed(p,line(turned),64,out);
        assert(!p.learned()&&!p.rotated());
    }
    OrientedPositionPipe sentio;sentio.reset(Model::Spectrum);Bytes sout;
    feed(sentio,":255 255 0 0 0 0 255 255\n",1,sout);
    assert(sentio.type()==Model::Sentio&&!sentio.rotated());
    // LED rotations: all 64 squares, double rotation, all 81 RGB corners/colours.
    for(unsigned i=0;i<64;++i) {
        uint8_t mask[8]{}, r[8]{}, twice[8]{};mask[i/8]=uint8_t(1u<<(i%8));
        rotateMask180(mask,r);assert(r[(63-i)/8]==uint8_t(1u<<((63-i)%8)));
        rotateMask180(r,twice);assert(std::memcmp(mask,twice,8)==0);
        for(auto model:{Model::Certabo,Model::Spectrum}) {
            Leds leds;Bytes out;leds.feed(mask,8,model,collect,&out,true);
            if(model==Model::Certabo)assert(out==Bytes(r,r+8));
            else {uint8_t expected[247];rgbFromMask(r,expected);assert(out==Bytes(expected,expected+247));}
        }
    }
    uint8_t masks[16]={255,85,1,2,3,4,5,6,7,8,9,10,11,12,13,14}, expectedMasks[16];
    rotateMask180(masks,expectedMasks);rotateMask180(masks+8,expectedMasks+8);
    for(size_t chunk=1;chunk<=16;++chunk) {
        Leds mono;Bytes out;
        for(size_t i=0;i<16;i+=chunk)mono.feed(masks+i,std::min(chunk,16-i),Model::Certabo,collect,&out,true);
        assert(out==Bytes(expectedMasks,expectedMasks+16));
    }
    Leds partial;Bytes fresh;
    partial.feed(masks,3,Model::Certabo,collect,&fresh,true);assert(partial.pending()&&fresh.empty());
    partial.reset();partial.feed(masks+8,8,Model::Certabo,collect,&fresh,true);
    assert(fresh==Bytes(expectedMasks+8,expectedMasks+16));
    uint8_t rgb[247]{}, rotated[247]{}, twice[247]{};
    rgb[0]=255;rgb[1]=85;rgb[245]=13;rgb[246]=10;
    for(unsigned i=0;i<81;++i) {rgb[2+3*i]=uint8_t(i);rgb[3+3*i]=uint8_t(81-i);rgb[4+3*i]=123;}
    rotateRgb180(rgb,rotated);rotateRgb180(rotated,twice);assert(std::memcmp(rgb,twice,247)==0);
    assert(rotated[2]==80&&rotated[242]==0&&rotated[3]==1);
    for(size_t chunk:{size_t(20),size_t(253)}) {
        Leds automatic;Bytes out;
        for(size_t i=0;i<247;i+=chunk)automatic.feed(rgb+i,std::min(chunk,247-i),Model::Spectrum,collect,&out,true);
        assert(out==Bytes(rotated,rotated+247));
    }
    Leds partialRgb;Bytes freshRgb;
    partialRgb.feed(rgb,20,Model::Spectrum,collect,&freshRgb,true);assert(partialRgb.pending());
    partialRgb.reset();partialRgb.feed(rgb,247,Model::Spectrum,collect,&freshRgb,true);
    assert(freshRgb==Bytes(rotated,rotated+247));
    for(size_t chunk=1;chunk<=253;++chunk) {
        Leds leds;leds.input=LedInput::Rgb;Bytes out;
        for(size_t i=0;i<247;i+=chunk)leds.feed(rgb+i,std::min(chunk,247-i),Model::Spectrum,collect,&out,true);
        assert(out==Bytes(rotated,rotated+247));
        leds.feed(rgb,247,Model::Spectrum,collect,&out,true);assert(out.size()==494);
        uint8_t off[8]{};leds.input=LedInput::Auto;
        leds.feed(off,8,Model::Spectrum,collect,&out,true);
        for(size_t j=494+2;j<494+245;++j)assert(out[j]==0);
    }
    // Reversal can create a reserved FF55 pair across RGB triples; escape it.
    uint8_t marker[247]{};marker[0]=255;marker[1]=85;marker[245]=13;marker[246]=10;
    marker[4+3*80]=255;marker[2+3*79]=85;
    Leds escaped;Bytes e;escaped.feed(marker,247,Model::Spectrum,collect,&e,true);
    assert(e[4]==254&&e[5]==85);
    OrientationBlink blink;bool on=false;blink.start(UINT32_MAX-50);
    assert(blink.next(UINT32_MAX-50,on)&&on);
    assert(!blink.next(UINT32_MAX-1,on));
    assert(blink.next(69,on)&&!on);assert(blink.next(189,on)&&on);
    assert(blink.next(309,on)&&!on&&!blink.active());
    blink.start(0);blink.reset();assert(!blink.active());
    // Oversized rotated records fail explicitly instead of emitting unrotated positions.
    OrientedPositionPipe large;large.reset(Model::Certabo);Bytes discarded;
    for(int i=0;i<3;++i)feed(large,line(normal),64,discarded);
    for(int i=0;i<3;++i)feed(large,line(turned),64,discarded);
    feed(large,":"+std::string(3000,' '),64,discarded);assert(large.overflow());
}
} // namespace orientation_test
