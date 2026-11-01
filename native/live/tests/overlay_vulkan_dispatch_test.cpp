// Exercise the private production helpers with dispatchable-handle stubs.
// No window, desktop input or Vulkan driver is created by this test.
#include "../overlay.cpp"
#include <cassert>
#include <iostream>

namespace {
struct Handle {void* dispatch;};
Handle deviceObject{reinterpret_cast<void*>(0x1000)};
Handle commandObjects[3]{};
VkDevice expectedDevice=reinterpret_cast<VkDevice>(&deviceObject);
VkCommandPool expectedPool=reinterpret_cast<VkCommandPool>(0x2000);
VkResult allocationResult=VK_SUCCESS,loaderResult=VK_SUCCESS,waitResult=VK_SUCCESS;
unsigned allocationCalls=0,loaderCalls=0,freeCalls=0,freedCount=0,waitCalls=0,instanceLookups=0,deviceLookups=0;
int loaderFailureIndex=-1;
VkFence expectedFence=reinterpret_cast<VkFence>(0x3000);
VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* commands) {
    assert(device==expectedDevice && info->commandPool==expectedPool && info->commandBufferCount==3);
    ++allocationCalls;
    if(allocationResult!=VK_SUCCESS)return allocationResult;
    for(unsigned i=0;i<3;++i){commandObjects[i].dispatch=nullptr;commands[i]=reinterpret_cast<VkCommandBuffer>(&commandObjects[i]);}
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Loader(VkDevice device,void* command) {
    assert(device==expectedDevice);
    const unsigned index=loaderCalls++;
    if(int(index)==loaderFailureIndex)return loaderResult;
    static_cast<Handle*>(command)->dispatch=deviceObject.dispatch;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Free(VkDevice device,VkCommandPool pool,uint32_t count,const VkCommandBuffer* commands) {
    assert(device==expectedDevice && pool==expectedPool && count==3);
    for(unsigned i=0;i<count;++i)assert(commands[i]==reinterpret_cast<VkCommandBuffer>(&commandObjects[i]));
    ++freeCalls;freedCount=count;
}
VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice device,uint32_t count,const VkFence* fences,VkBool32 all,uint64_t timeout) {
    assert(device==expectedDevice && count==1 && *fences==expectedFence && all==VK_TRUE && timeout==1000000000ull);
    ++waitCalls;return waitResult;
}
VKAPI_ATTR void VKAPI_CALL Function() {}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceFunction(VkDevice device,const char* name) {
    assert(device==expectedDevice && std::strncmp(name,"vkGetPhysicalDevice",19)!=0);
    assert(std::strcmp(name,"vkEnumeratePhysicalDevices")!=0 && std::strcmp(name,"vkDestroySurfaceKHR")!=0);
    ++deviceLookups;return Function;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceFunction(VkInstance,const char* name) {
    assert(std::strncmp(name,"vkGetPhysicalDevice",19)==0 || std::strcmp(name,"vkEnumeratePhysicalDevices")==0 || std::strcmp(name,"vkDestroySurfaceKHR")==0);
    ++instanceLookups;return Function;
}
}

int main() {
    using namespace camera_overlay;
    Instance instance{};instance.gipa=InstanceFunction;
    auto device=std::make_unique<Device>();device->handle=expectedDevice;device->instance=&instance;
    device->AllocateCommandBuffers=Allocate;device->FreeCommandBuffers=Free;
    device->set_loader_data=Loader;device->WaitForFences=Wait;device->gdpa=DeviceFunction;
    auto* d=device.get();devices[Key(expectedDevice)]=std::move(device);
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool=expectedPool;info.commandBufferCount=3;
    VkCommandBuffer commands[3]{};
    assert(AllocateOverlayCommandBuffers(expectedDevice,&info,commands)==VK_SUCCESS);
    assert(allocationCalls==1 && loaderCalls==3 && !freeCalls);
    for(const auto& object:commandObjects)assert(object.dispatch==deviceObject.dispatch);

    allocationResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;loaderCalls=0;
    assert(AllocateOverlayCommandBuffers(expectedDevice,&info,commands)==allocationResult);
    assert(!loaderCalls && !freeCalls); // No loader callback after failed allocation.
    allocationResult=VK_SUCCESS;loaderFailureIndex=1;loaderResult=VK_ERROR_INITIALIZATION_FAILED;
    assert(AllocateOverlayCommandBuffers(expectedDevice,&info,commands)==loaderResult);
    assert(loaderCalls==2 && freeCalls==1 && freedCount==3);
    for(auto command:commands)assert(command==VK_NULL_HANDLE);
    loaderFailureIndex=-1;d->set_loader_data=nullptr;
    assert(AllocateOverlayCommandBuffers(expectedDevice,&info,commands)==VK_SUCCESS);
    for(const auto& object:commandObjects)assert(object.dispatch==deviceObject.dispatch); // Old-loader parent dispatch fallback.

    // Only ImGui's private dispatch wraps allocation. Application queries keep
    // the next layer function; instance functions never go through gdpa.
    assert(BackendFunction("vkAllocateCommandBuffers",d)==reinterpret_cast<PFN_vkVoidFunction>(AllocateOverlayCommandBuffers));
    for(const char* name:{"vkGetPhysicalDeviceMemoryProperties","vkGetPhysicalDeviceSurfaceFormatsKHR","vkEnumeratePhysicalDevices","vkDestroySurfaceKHR"})
        assert(BackendFunction(name,d)==Function);
    assert(instanceLookups==4 && deviceLookups==0);
    assert(BackendFunction("vkCreateImage",d)==Function && deviceLookups==1);
    assert(Intercept("vkAllocateCommandBuffers")==nullptr);
    assert(CameraGetDeviceProcAddr(expectedDevice,"vkAllocateCommandBuffers")==Function);

    // Every completion policy waits at most once on exactly our submitted
    // frame fence; the hidden ordinary editor path never adds a wait.
    Frame frame{};frame.fence=expectedFence;
    for(bool panels:{false,true})for(bool capture:{false,true})for(bool diagnostic:{false,true}) {
        waitCalls=0;
        assert(CompleteOverlaySubmission(*d,frame,panels,capture,diagnostic)==VK_SUCCESS);
        assert(waitCalls==unsigned(panels || capture || diagnostic));
    }
    waitResult=VK_TIMEOUT;waitCalls=0;
    assert(CompleteOverlaySubmission(*d,frame,true,true,false)==VK_TIMEOUT && waitCalls==1);
    Swapchain swap{};swap.device=d;
    overlay_available=true;wants_mouse=true;wants_keyboard=true;text_input=true;window_focused=true;
    physical_keys[SDL_SCANCODE_LCTRL]=true;keyboard_state[SDL_SCANCODE_LCTRL]=false;
    DisableOverlay(swap);
    assert(swap.failed && !overlay_available && !wants_mouse && !wants_keyboard && !text_input);
    assert(keyboard_state[SDL_SCANCODE_LCTRL]); // Return physical polling to the game.
    bool rejected=false;
    try{CheckBackendResult(VK_ERROR_INITIALIZATION_FAILED);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected);CheckBackendResult(VK_SUCCESS);
    devices.clear();
    std::cout<<"Private Vulkan dispatch: all command buffers initialized, allocator failures cleaned, instance lookup scopes, visible/hidden/capture fence policies and input release passed\n";
}
