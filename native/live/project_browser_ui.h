#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cs2_live_ui {

// Deliberately locale-independent: ASCII, Latin-1 and the Cyrillic alphabet
// (including Ё) fold to lowercase; other UTF-8 sequences remain literal. This
// does not change the process locale used by the game or numeric controls.
inline std::string ProjectSearchFold(std::string text) {
  for (std::size_t i=0;i<text.size();++i) {
    const auto first=static_cast<unsigned char>(text[i]);
    if(first>='A' && first<='Z')text[i]=char(first+'a'-'A');
    else if(i+1<text.size()) {
      const auto second=static_cast<unsigned char>(text[i+1]);
      if((first==0xc3 || first==0xd0) && (second&0xc0)==0x80) {
        unsigned code=((first&0x1f)<<6)|(second&0x3f);
        if((code>=0x00c0 && code<=0x00d6)||(code>=0x00d8 && code<=0x00de)||
           (code>=0x0410 && code<=0x042f))code+=0x20;
        else if(code>=0x0400 && code<=0x040f)code+=0x50;
        text[i]=char(0xc0|(code>>6));text[i+1]=char(0x80|(code&0x3f));++i;
      }
    }
  }
  return text;
}

inline std::string ProjectDemoFilename(const std::string& path) {
  const auto slash=path.find_last_of("/\\");
  return slash==std::string::npos?path:path.substr(slash+1);
}

// Rendering consumes the bounded host snapshot only. Cache folded strings and
// ordering until the snapshot or filters change, never scan files from the UI.
class ProjectBrowserIndex {
 public:
  using Json=nlohmann::json;
  enum class Sort { Recent, Name };

  const std::vector<std::size_t>& Filter(const Json& entries,const std::string& query,
                                       bool compatible_only,Sort sort) {
    bool changed=entries!=snapshot_;
    if(changed) {
      snapshot_=entries;rows_.clear();
      if(snapshot_.is_array())for(std::size_t i=0;i<snapshot_.size();++i) {
        const auto& entry=snapshot_[i];
        if(!entry.is_object())continue;
        const auto id=Text(entry,"id");if(id.empty())continue;
        const auto title=Text(entry,"title",id);
        const auto demo=entry.find("demo");
        const auto filename=demo!=entry.end()?ProjectDemoFilename(Text(*demo,"path")):std::string();
        rows_.push_back({i,id,ProjectSearchFold(title),ProjectSearchFold(title)+"\n"+
                         ProjectSearchFold(id)+"\n"+ProjectSearchFold(filename),
                         std::max(Timestamp(entry,"saved_at_ns"),Timestamp(entry,"opened_at_ns")),
                         entry.value("compatible",Json(false))==true});
      }
    }
    if(!changed && query==query_ && compatible_only==compatible_only_ && sort==sort_)return visible_;
    query_=query;compatible_only_=compatible_only;sort_=sort;visible_.clear();
    const auto folded=ProjectSearchFold(query);
    for(std::size_t i=0;i<rows_.size();++i)
      if((!compatible_only || rows_[i].compatible) &&
         (folded.empty() || rows_[i].search.find(folded)!=std::string::npos))visible_.push_back(i);
    std::sort(visible_.begin(),visible_.end(),[&](std::size_t a,std::size_t b) {
      const auto& left=rows_[a];const auto& right=rows_[b];
      if(sort==Sort::Recent && left.recent!=right.recent)return left.recent>right.recent;
      if(sort==Sort::Name && left.name!=right.name)return left.name<right.name;
      return left.id<right.id;
    });
    return visible_;
  }
  const Json& Entry(std::size_t row)const{return snapshot_[rows_[row].source];}
  std::size_t Size()const{return rows_.size();}
  bool Contains(const std::string& id)const {
    return std::any_of(rows_.begin(),rows_.end(),[&](const Row& row){return row.id==id;});
  }

 private:
  struct Row {std::size_t source;std::string id,name,search;std::int64_t recent;bool compatible;};
  static std::string Text(const Json& entry,const char* field,const std::string& fallback={}) {
    const auto found=entry.find(field);
    return found!=entry.end() && found->is_string()?found->get<std::string>():fallback;
  }
  static std::int64_t Timestamp(const Json& entry,const char* field) {
    const auto found=entry.find(field);
    if(found==entry.end() || !found->is_number_integer())return 0;
    if(found->is_number_unsigned())return std::int64_t(std::min(found->get<std::uint64_t>(),std::uint64_t(std::numeric_limits<std::int64_t>::max())));
    return std::max(std::int64_t(0),found->get<std::int64_t>());
  }
  Json snapshot_;
  std::vector<Row> rows_;
  std::vector<std::size_t> visible_;
  std::string query_;
  bool compatible_only_=false;
  Sort sort_=Sort::Recent;
};
} // namespace cs2_live_ui
