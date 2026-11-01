#include "../weapon_cosmetics.h"
#include <cassert>
#include <iostream>
using Cosmetics=cs2_live::WeaponCosmetics;
using Json=nlohmann::json;
struct Engine {
    std::vector<Cosmetics::Weapon> weapons;unsigned writes=0,snapshots=0;bool expired=false,reject=false,hide=false;
    Engine(){Cosmetics::Weapon w;w.actor.steamId="76561198012345678";w.actor.nickname="same name";w.actor.pawnHandle=32775;w.actor.controllerHandle=32771;w.actor.instance=0x1000;w.actor.generation=1;w.actor.team=2;w.handle=98320;w.instance=0x2000;w.itemDef=7;w.ready=w.hudReady=w.active=true;weapons.push_back(w);w.actor.steamId="76561198012345679";w.actor.pawnHandle=65544;w.actor.instance=0x3000;w.handle=131089;w.instance=0x4000;weapons.push_back(w);}
    bool Current(const Cosmetics::Weapon& value)const{if(expired)return false;for(const auto& w:weapons)if(w.handle==value.handle && w.instance==value.instance && w.actor.pawnHandle==value.actor.pawnHandle && w.actor.steamId==value.actor.steamId && w.actor.generation==value.actor.generation)return true;return false;}
    Cosmetics::Callbacks Callbacks(){Cosmetics::Callbacks cb;cb.available=true;cb.reason="test adapter";cb.weapons=[&]{++snapshots;return hide?std::vector<Cosmetics::Weapon>{}:weapons;};cb.current=[&](const auto& w){return Current(w);};cb.resolve=[](const Json& entry){Cosmetics::Paint p;p.kit=entry["paint_kit"].get<unsigned>();if(entry["item_def"]!=7 || (p.kit!=180 && p.kit!=302))throw std::invalid_argument("not a catalog pair");p.seed=entry.value("seed",0u);p.wear=entry.value("wear",0.01f);return p;};cb.attributes=[&](const auto& w,const auto& attrs){assert(Current(w));++writes;if(reject)return false;for(auto& live:weapons)if(live.handle==w.handle)live.attributes=attrs;return true;};return cb;}
    Json Desired(unsigned kit=180)const{return Json::array({{{"steam_id",weapons[0].actor.steamId},{"item_def",7},{"paint_kit",kit},{"seed",42},{"wear",0.1}}});}
};
template<class Fn>void Reject(Fn fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}assert(failed);}
static void ScopeAndRestore() {
    Engine e;Cosmetics c;c.Bind(e.Callbacks());c.ValidateRequest(e.Desired(),true);c.Apply(e.Desired());
    assert(e.writes==1 && e.weapons[0].attributes==Cosmetics::Attributes({180.f,42.f,0.1f}) && e.weapons[1].attributes==Cosmetics::Attributes{});
    assert(c.State()["weapons"][0]["applied"]==true && c.State()["weapons"][1]["applied"]==false);
    // Pending jobs and attribute readback are independent. Unknown readiness
    // blocks capture; a completed queue never changes the applied semantics.
    assert(c.State()["weapons"][0]["materials_pending"]==true);
    e.weapons[0].worldMaterialJobs=1;e.weapons[0].hudMaterialJobs=0;c.Refresh(true);
    assert(c.State()["weapons"][0]["materials_pending"]==true && c.State()["weapons"][0]["applied"]==true);
    e.weapons[0].worldMaterialJobs=0;c.Refresh(true);assert(c.State()["weapons"][0]["materials_pending"]==false);
    const auto snapshots=e.snapshots;c.Refresh(true);assert(e.writes==1 && e.snapshots==snapshots+1);c.Apply(Json::array());assert(e.writes==2 && e.weapons[0].attributes==Cosmetics::Attributes{});
    assert(c.Capabilities()["render_verified_1p"]==false && c.Capabilities()["render_verified_3p"]==false);
}
static void BaselineSeekAndExternalChanges() {
    Engine e;Cosmetics c;e.weapons[0].attributes={12.f,std::nullopt,0.3f};c.Bind(e.Callbacks());c.Apply(e.Desired());
    // Preserve a new demo baseline when the network reinstalls it on this item.
    e.weapons[0].attributes={13.f,2.f,0.4f};c.Refresh(true);c.Apply(Json::array());assert(e.weapons[0].attributes==Cosmetics::Attributes({13.f,2.f,0.4f}));
    c.Apply(e.Desired());e.weapons[0].attributes[1]=900.f;c.Apply(Json::array());
    assert(e.weapons[0].attributes==Cosmetics::Attributes({13.f,900.f,0.4f})); // Unowned external delta survives reset.
    c.Apply(e.Desired());e.hide=true;c.Refresh(false);assert(c.State()["stale"]==true);e.hide=false;c.Refresh(false);assert(e.weapons[0].attributes==Cosmetics::Attributes({13.f,900.f,0.4f}));
}
static void LifecycleAndTransfer() {
    Engine e;Cosmetics c;c.Bind(e.Callbacks());c.Apply(e.Desired());const auto old=e.weapons[0].handle;
    e.weapons[0].handle+=1<<15;e.weapons[0].instance+=0x100;e.weapons[0].attributes={15.f,4.f,0.5f};c.Refresh(true);c.Apply(Json::array());
    assert(e.weapons[0].handle!=old && e.weapons[0].attributes==Cosmetics::Attributes({15.f,4.f,0.5f}));
    c.Apply(e.Desired());e.weapons[0].actor=e.weapons[1].actor;c.Refresh(true);assert(e.weapons[0].attributes==Cosmetics::Attributes({15.f,4.f,0.5f}));
    const auto count=e.writes;e.expired=true;c.Refresh(false);assert(e.writes==count);
}
static void BoundedFailureAndValidation() {
    Engine e;Cosmetics c;e.reject=true;c.Bind(e.Callbacks());c.Apply(e.Desired());c.Refresh(true);c.Refresh(true);assert(e.writes==1 && c.State()["weapons"][0]["applied"]==false);
    e.reject=false;c.Apply(e.Desired(302));assert(e.writes==2 && c.State()["weapons"][0]["applied"]==true);c.Apply(Json::array());assert(e.weapons[0].attributes==Cosmetics::Attributes{});
    auto d=e.Desired();d[0]["item_def"]=16;Reject([&]{c.ValidateRequest(d,true);});
    d=e.Desired();d[0]["steam_id"]="76561198012345680";Reject([&]{c.ValidateRequest(d,true);});
    for(const auto& pair:std::vector<std::pair<std::string,Json>>{{"seed",1001},{"seed",true},{"wear",1.01},{"wear",true},{"paint_kit",0},{"paint_kit",180.0},{"raw_address",123}}){d=e.Desired();d[0][pair.first]=pair.second;Reject([&]{cs2_live::ValidateWeaponPaints(d);});}
    d=e.Desired();d.push_back(d[0]);Reject([&]{cs2_live::ValidateWeaponPaints(d);});
    Cosmetics missing;Reject([&]{missing.Apply(e.Desired());});
}
static void TransferWithBothPlayersOverridden() {
    Engine e;Cosmetics c;c.Bind(e.Callbacks());auto desired=e.Desired();
    desired.push_back({{"steam_id",e.weapons[1].actor.steamId},{"item_def",7},{"paint_kit",302},{"seed",123},{"wear",0.2}});
    c.Apply(desired);assert(e.writes==2);
    e.weapons[0].actor=e.weapons[1].actor;c.Refresh(true);
    assert(e.weapons[0].attributes==Cosmetics::Attributes({302.f,123.f,0.2f}));
    c.Apply(Json::array());assert(e.weapons[0].attributes==Cosmetics::Attributes{} && e.weapons[1].attributes==Cosmetics::Attributes{});
}
static void MapGenerationForgetsAbsentJournal() {
    Engine e;Cosmetics c;c.Bind(e.Callbacks());c.Apply(e.Desired());assert(c.State()["owned_overrides"]==1);
    // A fresh system generation proves this missing former map object is no
    // longer a restoration target. Its pointer is never touched again.
    e.weapons.erase(e.weapons.begin());++e.weapons[0].actor.generation;c.Refresh(false);
    assert(c.State()["owned_overrides"]==0);c.Bind(e.Callbacks());
}
struct DetachedEngine:Engine {
    using Result=Cosmetics::RestoreResult;
    std::optional<Cosmetics::Weapon> ground;Result result=Result::Restored;unsigned restoreCalls=0,restoreWrites=0;
    Cosmetics::Callbacks Callbacks() {
        auto cb=Engine::Callbacks();cb.restoreDetached=[&](const auto& weapon,const auto& original,const auto& last) {
            ++restoreCalls;
            if(result!=Result::Restored)return result;
            assert(ground && ground->handle==weapon.handle && ground->instance==weapon.instance);
            auto target=ground->attributes;
            for(unsigned index=0;index<3;++index)if(target[index]==last[index])target[index]=original[index];
            if(target!=ground->attributes){++restoreWrites;ground->attributes=target;}
            return Result::Restored;
        };return cb;
    }
    void Drop(){ground=weapons.front();weapons.erase(weapons.begin());}
};
static void DroppedWorldOnlyRestore() {
    DetachedEngine e;Cosmetics c;e.weapons[0].attributes={12.f,std::nullopt,0.3f};c.Bind(e.Callbacks());const auto desired=e.Desired();c.Apply(desired);
    e.Drop();e.ground->hudReady=false;e.ground->hudHandle=0;e.ground->hudInstance=0;
    // An outside seed change survives restoring the attributes owned by us.
    e.ground->attributes[1]=900.f;const auto snapshots=e.snapshots;c.Refresh(true);
    assert(e.restoreCalls==1 && e.restoreWrites==1 && e.writes==1 && e.snapshots==snapshots+2);
    assert(e.ground->attributes==Cosmetics::Attributes({12.f,900.f,0.3f}));
    assert(c.State()["owned_overrides"]==0 && c.State()["restore_pending"].empty());
    // The former selected player's still-enabled entry never follows the gun
    // to another player, and that player's new baseline remains the original.
    e.ground->actor=e.weapons[0].actor;e.ground->ready=e.ground->hudReady=true;e.weapons.push_back(*e.ground);c.Refresh(true);
    assert(e.writes==1 && e.weapons.back().attributes==Cosmetics::Attributes({12.f,900.f,0.3f}));
    c.Apply(Json::array());assert(e.writes==1);
}
static void DetachedDeferredExpiredAndRejected() {
    DetachedEngine e;Cosmetics c;c.Bind(e.Callbacks());const auto desired=e.Desired();c.Apply(desired);e.Drop();e.result=DetachedEngine::Result::Deferred;
    c.Refresh(false);c.Refresh(false);
    assert(e.restoreCalls==2 && e.restoreWrites==0 && c.State()["owned_overrides"]==1 && c.State()["restore_pending"].size()==1);
    // An unreadable identity never causes a guessed restore or journal loss.
    Reject([&]{c.Bind(e.Callbacks());});e.result=DetachedEngine::Result::Expired;c.Refresh(false);
    assert(c.State()["owned_overrides"]==0 && e.restoreWrites==0);c.Bind(e.Callbacks());

    DetachedEngine rejected;Cosmetics blocked;blocked.Bind(rejected.Callbacks());auto selection=rejected.Desired();blocked.Apply(selection);rejected.Drop();rejected.result=DetachedEngine::Result::Rejected;
    blocked.Refresh(false);blocked.Refresh(false);blocked.Refresh(false);
    assert(rejected.restoreCalls==1 && rejected.restoreWrites==0 && blocked.State()["owned_overrides"]==1 && blocked.State()["restore_pending"].size()==1);
    // A changed request permits one new explicit attempt, not frame retries.
    blocked.Apply(Json::array());blocked.Refresh(false);assert(rejected.restoreCalls==2);
    rejected.result=DetachedEngine::Result::Restored;blocked.Apply(selection);
    assert(rejected.restoreCalls==3 && rejected.restoreWrites==1 && blocked.State()["owned_overrides"]==0);
}
static void DetachedNewGenerationAvoidsCallback() {
    DetachedEngine e;Cosmetics c;c.Bind(e.Callbacks());c.Apply(e.Desired());e.Drop();++e.weapons[0].actor.generation;c.Refresh(false);
    assert(e.restoreCalls==0 && e.restoreWrites==0 && c.State()["owned_overrides"]==0);
}
int main(){ScopeAndRestore();BaselineSeekAndExternalChanges();LifecycleAndTransfer();BoundedFailureAndValidation();TransferWithBothPlayersOverridden();MapGenerationForgetsAbsentJournal();DroppedWorldOnlyRestore();DetachedDeferredExpiredAndRejected();DetachedNewGenerationAvoidsCallback();std::cout<<"weapon cosmetics tests passed\n";}
