#pragma once
#include <cstdint>
#include <cstring>
namespace solo {
// A new request replaces the previous PIN; monotonic time bounds its lifetime.
struct AccessPin {
    uint16_t value=0; uint64_t expires=0;
    bool expired(uint64_t now) const { return now>=expires; }
    void rotate(uint32_t random,uint64_t now) {
        value=uint16_t((uint32_t(value)+1+random%9999)%10000);
        expires=now+3600000;
    }
};
// Global attempts, independent of browser/IP. Persist failures before replying.
struct AccessRate {
    unsigned failures=0; uint64_t next=0;
    void boot(unsigned stored,uint64_t now) { failures=stored; next=now+(stored>=10?3600000:0); }
    uint32_t wait(uint64_t now) const { return now<next?uint32_t((next-now+999)/1000):0; }
    void result(bool correct,uint64_t now) { failures=correct?0:(failures<10?failures+1:10); next=now+(failures>=10?3600000:1000); }
};
struct AccessSessions {
    struct Session { char token[33]{}; uint64_t touched=0; } slots[4];
    bool check(const char *token,uint64_t now,bool touch=false) {
        if(!token || strlen(token)!=32) return false;
        for(auto &s:slots) if(!strcmp(s.token,token)) {
            if(now-s.touched>=300000) { s.token[0]=0; return false; }
            if(touch) s.touched=now;
            return true;
        }
        return false;
    }
    uint32_t remaining(const char *token,uint64_t now) {
        if(!check(token,now)) return 0;
        for(auto &s:slots) if(!strcmp(s.token,token)) return uint32_t((300000-(now-s.touched)+999)/1000);
        return 0;
    }
    void add(const char *token,uint64_t now) {
        Session *target=&slots[0];
        for(auto &s:slots) { if(!*s.token || now-s.touched>=300000) { target=&s; break; } if(s.touched<target->touched) target=&s; }
        strcpy(target->token,token); target->touched=now;
    }
    void remove(const char *token) { for(auto &s:slots) if(!strcmp(s.token,token)) s.token[0]=0; }
};
}
