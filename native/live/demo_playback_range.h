#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace cs2_live {
// Session navigation bounds are deliberately separate from the camera project
// and its undo history. Authoritative DEM_FileInfo supplies the outer bounds.
class DemoPlaybackRange {
public:
    using Json=nlohmann::json;
    void Bind(int total,std::uint64_t recording) {
        if(total<=0)return;
        if(recording_!=recording || total_!=total) {
            recording_=recording;total_=total;in_=0;out_=total;enabled_=false;
        }
    }
    void Update(const Json& patch) {
        if(!total_)throw std::runtime_error("demo playback limits require authoritative demo duration metadata");
        if(!patch.is_object() || patch.empty())throw std::invalid_argument("demo playback range requires a nonempty object");
        bool enabled=enabled_;int in=in_,out=out_;
        for(auto item=patch.begin();item!=patch.end();++item) {
            if(item.key()=="enabled") {
                if(!item->is_boolean())throw std::invalid_argument("demo playback enabled must be boolean");
                enabled=item->get<bool>();
            } else if(item.key()=="in_tick" || item.key()=="out_tick") {
                if(!item->is_number_integer() || (item->is_number_unsigned() && item->get<std::uint64_t>()>std::uint64_t(total_)))
                    throw std::invalid_argument("demo playback tick must be an integer inside this recording");
                const auto value=item->get<std::int64_t>();
                if(value<0 || value>total_)throw std::invalid_argument("demo playback tick is outside this recording");
                (item.key()=="in_tick"?in:out)=static_cast<int>(value);
            } else throw std::invalid_argument("unsupported demo playback range field: "+item.key());
        }
        if(in>=out)throw std::invalid_argument("demo playback In must precede Out by at least one tick");
        enabled_=enabled;in_=in;out_=out;
    }
    bool Enabled()const{return enabled_&&total_>0;}
    bool Contains(int tick)const{return tick>=in_&&tick<out_;}
    int In()const{return in_;}
    Json State()const {
        Json result={{"available",total_>0},{"enabled",Enabled()},{"in_tick",in_},{"out_tick",out_}};
        if(!total_)result["reason"]="Authoritative demo duration metadata is unavailable";
        return result;
    }
private:
    std::uint64_t recording_=0;
    int total_=0,in_=0,out_=0;
    bool enabled_=false;
};
} // namespace cs2_live
