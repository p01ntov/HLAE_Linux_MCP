#include "../overlay_blur.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

static void Check(VkResult r) { if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan result "+std::to_string(r)); }
static unsigned Memory(VkPhysicalDevice physical,unsigned bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for(unsigned i=0;i<p.memoryTypeCount;++i)if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    throw std::runtime_error("no test memory type");
}
static float Half(unsigned short h) {
    const auto exp=(h>>10)&31,frac=h&1023;
    float v=exp ? std::ldexp(1.f+frac/1024.f,int(exp)-15) : std::ldexp(float(frac),-24);
    return h&0x8000 ? -v : v;
}
static double Decode(unsigned char n) { const double v=n/255.;return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4); }
static double EncodeLinear(double v) { v=std::clamp(v,0.,1.);return v<=.0031308?12.92*v:1.055*std::pow(v,1./2.4)-.055; }
static double DecodeEncoded(double v) { return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4); }
int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="offline blur pixels";app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;VkInstance instance{};Check(vkCreateInstance(&ic,nullptr,&instance));
    unsigned count=0;Check(vkEnumeratePhysicalDevices(instance,&count,nullptr));assert(count);
    std::vector<VkPhysicalDevice> physicals(count);Check(vkEnumeratePhysicalDevices(instance,&count,physicals.data()));auto physical=physicals[0];
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);std::cout<<props.deviceName<<"\n";
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    unsigned family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;assert(family<count);
    float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;VkDevice device{};Check(vkCreateDevice(physical,&dc,nullptr,&device));VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lc.bindingCount=1;lc.pBindings=&binding;VkDescriptorSetLayout layout{};Check(vkCreateDescriptorSetLayout(device,&lc,nullptr,&layout));
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;pc.maxSets=16;pc.poolSizeCount=1;pc.pPoolSizes=&poolSize;VkDescriptorPool descriptors{};Check(vkCreateDescriptorPool(device,&pc,nullptr,&descriptors));
    unsigned adds=0,removes=0;
    cs2_camera_blur::InitInfo info{};info.device=device;info.physical=physical;info.gdpa=vkGetDeviceProcAddr;info.getMemoryProperties=vkGetPhysicalDeviceMemoryProperties;info.getFormatProperties=vkGetPhysicalDeviceFormatProperties;info.extent={64,32};info.sourceFormat=VK_FORMAT_R8G8B8A8_UNORM;info.imageCount=2;
    info.AddTexture=[&](VkSampler sampler,VkImageView view,VkImageLayout imageLayout){VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&layout;VkDescriptorSet set{};Check(vkAllocateDescriptorSets(device,&alloc,&set));VkDescriptorImageInfo image{sampler,view,imageLayout};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;vkUpdateDescriptorSets(device,1,&write,0,nullptr);++adds;return set;};
    info.RemoveTexture=[&](VkDescriptorSet set){Check(vkFreeDescriptorSets(device,descriptors,1,&set));++removes;};
    cs2_camera_blur::Backdrop blur;assert(blur.Initialize(info));assert(blur.Texture(0) && blur.Texture(1));assert(!blur.Texture(2));
    const unsigned width=info.extent.width,height=info.extent.height,pixels=width*height;const VkDeviceSize outputOffset=pixels*4,bytes=pixels*12;
    VkImageCreateInfo imageCreate{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imageCreate.imageType=VK_IMAGE_TYPE_2D;imageCreate.format=info.sourceFormat;imageCreate.extent={width,height,1};imageCreate.mipLevels=imageCreate.arrayLayers=1;imageCreate.samples=VK_SAMPLE_COUNT_1_BIT;imageCreate.tiling=VK_IMAGE_TILING_OPTIMAL;imageCreate.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;VkImage image{};Check(vkCreateImage(device,&imageCreate,nullptr,&image));
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image,&req);VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=req.size;allocate.memoryTypeIndex=Memory(physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VkDeviceMemory imageMemory{};Check(vkAllocateMemory(device,&allocate,nullptr,&imageMemory));Check(vkBindImageMemory(device,image,imageMemory,0));
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=bytes;bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer{};Check(vkCreateBuffer(device,&bc,nullptr,&buffer));vkGetBufferMemoryRequirements(device,buffer,&req);allocate.allocationSize=req.size;allocate.memoryTypeIndex=Memory(physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VkDeviceMemory bufferMemory{};Check(vkAllocateMemory(device,&allocate,nullptr,&bufferMemory));Check(vkBindBufferMemory(device,buffer,bufferMemory,0));void* mapped=nullptr;Check(vkMapMemory(device,bufferMemory,0,VK_WHOLE_SIZE,0,&mapped));
    VkCommandPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolCreate.queueFamilyIndex=family;poolCreate.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool commands{};Check(vkCreateCommandPool(device,&poolCreate,nullptr,&commands));VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commands;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer command{};Check(vkAllocateCommandBuffers(device,&ca,&command));
    auto barrier=[&](VkImage target,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=before;b.newLayout=after;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=target;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(command,from,to,0,0,nullptr,0,nullptr,1,&b);};
    std::array<double,7> weights{};double sum=0;for(int i=0;i<=6;++i){weights[i]=std::exp(-i*i/8.);sum+=weights[i]*(i?2:1);}for(auto& w:weights)w/=sum;
    for(unsigned test=0;test<11;++test) {
        if(test==8) {
            blur.Destroy();info.encodedDimAlpha=160.f/255;
            assert(blur.Initialize(info));
        }
        std::vector<unsigned char> data(pixels*4,0);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto offset=(y*width+x)*4;data[offset+3]=255;
            if(test%3==0)for(unsigned c=0;c<3;++c)data[offset+c]=128;
            if(test%3==1 && x==width/2 && y==height/2)for(unsigned c=0;c<3;++c)data[offset+c]=255;
            if(test%3==2){data[offset]=x*255/(width-1);data[offset+1]=y*255/(height-1);data[offset+2]=(x/4+y/4)%2?255:0;}
        }
        std::memcpy(mapped,data.data(),data.size());
        Check(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(command,&begin));
        barrier(image,test?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,test?VK_ACCESS_MEMORY_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyBufferToImage(command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        assert(blur.Record(command,test%2,image,VK_IMAGE_LAYOUT_GENERAL));auto result=blur.OutputImage(test%2);
        barrier(result,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy.bufferOffset=outputOffset;vkCmdCopyImageToBuffer(command,result,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        barrier(result,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        Check(vkEndCommandBuffer(command));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;Check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));Check(vkQueueWaitIdle(queue));
        const auto* output=reinterpret_cast<const unsigned short*>(static_cast<unsigned char*>(mapped)+outputOffset);double maxError=0;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned c=0;c<3;++c) {
            double expected=0;
            for(int dy=-6;dy<=6;++dy)for(int dx=-6;dx<=6;++dx) {
                const unsigned sx=std::clamp(int(x)+dx,0,int(width)-1),sy=std::clamp(int(y)+dy,0,int(height)-1);
                expected+=weights[std::abs(dx)]*weights[std::abs(dy)]*Decode(data[(sy*width+sx)*4+c]);
            }
            if(test>=8)expected=DecodeEncoded(EncodeLinear(expected)*(1-info.encodedDimAlpha));
            maxError=std::max(maxError,std::abs(expected-Half(output[(y*width+x)*4+c])));
        }
        std::cout<<"case "<<test<<" max linear RGB error "<<maxError<<"\n";assert(maxError<.0015);
    }
    blur.Destroy();assert(adds==removes);
    auto bad=info;bad.sourceFormat=VK_FORMAT_A2B10G10R10_UNORM_PACK32;assert(!blur.Initialize(bad));assert(!blur.Ready());assert(!blur.Error().empty());
    assert(blur.Initialize(info));blur.Destroy();assert(adds==removes);
    vkUnmapMemory(device,bufferMemory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bufferMemory,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,imageMemory,nullptr);vkDestroyCommandPool(device,commands,nullptr);vkDestroyDescriptorPool(device,descriptors,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    std::cout<<"Backdrop: SRGB decode, sigma=2 kernel, encoded dim, UV orientation, per-image reuse, format rejection, teardown OK\n";
}
