#pragma once
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>

namespace cs2_live {
// Only background DTO observation is paced. Mutation/readback and native
// publication work must explicitly bypass this gate; capture always does.
class LiveTelemetryCadence {
public:
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
    using Now=std::function<Time()>;
    static bool LiveOnly() {
        const auto* live=std::getenv("CS2_CAMERA_LIVE");
        const auto* capture=std::getenv("CS2_CAMERA_CAPTURE_COSMETICS");
        return live && std::strcmp(live,"1")==0 && (!capture || std::strcmp(capture,"1")!=0);
    }
    explicit LiveTelemetryCadence(Clock::duration interval,bool limited=LiveOnly(),Now now={})
        :interval_(interval),limited_(limited),now_(std::move(now)){}
    bool Due(bool immediate=false) {
        const auto now=NowValue();
        if(limited_ && !immediate && next_ && now<*next_)return false;
        next_=now+interval_;return true;
    }
    void Advance(){next_=NowValue()+interval_;}
    void Reset(){next_.reset();}
private:
    Time NowValue()const{return now_?now_():Clock::now();}
    Clock::duration interval_;bool limited_;Now now_;std::optional<Time> next_;
};
}
