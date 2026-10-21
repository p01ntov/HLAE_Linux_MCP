#include "../live_controller.h"
#include <cassert>
#include <iostream>
using namespace cs2_live;
int main(){
    setenv("CS2_CAMERA_LIVE","1",1);Controller c;unsigned reads=0;PlayerCosmetics::Callbacks cb;cb.actors=[&]{++reads;return std::vector<PlayerCosmetics::Actor>{};};c.SetCosmeticsBackend(std::move(cb));
    const auto frame=[&]{c.Frame(true,100,true,64,true,"",[](const std::string&){});};frame();
    const auto state=[&]{return Json::parse(c.StateJson());};const auto baseline=state()["project"];
    const auto command=[&](const char* method,Json params){c.EnqueueText(Json{{"method",method},{"params",params}}.dump().c_str());frame();assert(state().contains("error") && state()["project"]==baseline);};
    for(const char* method:{"set_player_weapon_paint","reset_player_weapon_paint","set_player_gloves","reset_player_gloves","apply_appearance_pack","reset_appearance","probe_appearance_pack"})command(method,Json::object());
    for(const char* key:{"weapon_paints","gloves"}){
        auto p=baseline;p["cosmetics"][key]=Json::array({{{"steam_id","76561198012345678"},{"item_def",7},{"paint_kit",180}}});command("set_project",p);
        bool failed=false;try{ReadProject(p);}catch(const std::exception&){failed=true;}assert(failed);
        p["cosmetics"][key]=Json::array();command("set_project",p);
    }
    for(const char* key:{"appearance","appearance_pack"}){auto p=baseline;p[key]={{"path","/tmp/pack"}};command("set_project",p);}
    assert(state()["weapon_cosmetics"]["weapons"].empty() && state()["glove_cosmetics"]["gloves"].empty());
    assert(state()["appearance"]["active"]==false && state()["appearance_capabilities"]["available"]==false);
    std::cout<<"Native production: weapon/glove/appearance methods and legacy imports reject before project mutation\n";
}
