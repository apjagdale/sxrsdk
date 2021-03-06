/* Copyright 2015 Samsung Electronics Co., LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/****
 *
 * VertexBuffer maintains a vertex data array with locations, normals,
 * colors and texcoords.
 *
 ****/
#include <engine/renderer/vulkan_renderer.h>
#include "vulkan_vertex_buffer.h"
#include "vulkan_shader.h"

namespace sxr {
    VulkanVertexBuffer::VulkanVertexBuffer(const char* layout_desc, int vertexCount)
            : VertexBuffer(layout_desc, vertexCount)
    {
        mVerticesMap.clear();
    }

    VulkanVertexBuffer::~VulkanVertexBuffer()
    {
        freeGPUResources();
    }

    void VulkanVertexBuffer::freeGPUResources()
    {
    }

    bool VulkanVertexBuffer::updateGPU(Renderer* renderer, IndexBuffer* ibuf, Shader* shader)
    {
        std::lock_guard<std::mutex> lock(mLock);
        VulkanRenderer* vkrender = static_cast<VulkanRenderer*>(renderer);
        const float* vertexData = getVertexData();
        if ((getVertexCount() == 0) || (vertexData == NULL))
        {
            LOGE("VertexBuffer::updateGPU no vertex data yet");
            return false;
        }
        generateVKBuffers(vkrender->getCore(),shader);
        if (ibuf)
        {
            ibuf->updateGPU(renderer);
        }
        return true;
    }
    const SXR_VK_Vertices* VulkanVertexBuffer::getVKVertices(Shader* shader)  {
        std::unordered_map<Shader*,std::shared_ptr<SXR_VK_Vertices>>::iterator it;
        if((it = mVerticesMap.find(shader)) == mVerticesMap.end()) {
            LOGE("vertex buffer not created");
            return NULL;
        }

        return (it->second).get();
    }
    void VulkanVertexBuffer::generateVKBuffers(VulkanCore* vulkanCore, Shader* shader)
    {

        if(mVerticesMap.find(shader) != mVerticesMap.end() && !isDirty())
            return;

        VkResult   err;
        bool   pass;
        std::shared_ptr<SXR_VK_Vertices> vertices(new SXR_VK_Vertices);
        // Our m_vertices member contains the types required for storing
        // and defining our vertex buffer within the graphics pipeline

        int i = 0;
        int stride = 0;
        shader->getVertexDescriptor().forEachEntry([this, &i, &vertices, &stride](const DataDescriptor::DataEntry &e)
                                                   {
                                                       LOGV("VertexBuffer::bindToShader find %s", e.Name);
                                                       const DataDescriptor::DataEntry* entry = find(e.Name);

                                                       if (!e.NotUsed)                             // shader uses this vertex attribute?
                                                       {
                                                           if ((entry != nullptr) && entry->IsSet) // mesh uses this vertex attribute?
                                                           {
                                                               VkVertexInputAttributeDescription binding;
                                                               binding.binding = SXR_VK_VERTEX_BUFFER_BIND_ID;
                                                               binding.location = e.Index;
                                                               LOGV("location %d attrMapping[i].offset %d , name %s", entry->Index, entry->Offset, entry->Name);
                                                               binding.format = getDataType(entry->Type); //float3
                                                               binding.offset = entry->Offset;
                                                               stride+= entry->Size;
                                                               vertices->vi_attrs.push_back(binding);
                                                               i++;
                                                           }
                                                           else                                // mesh uses attribute but shader does not
                                                           {
                                                               LOGW("entry is not present %s", e.Name);

                                                           }

                                                       }
                                                   });


        // Create our buffer object.
        VkDevice& device = vulkanCore->getDevice();
        VkBufferCreateInfo bufferCreateInfo = {};
        int bufferByteSize = getVertexCount() * mTotalSize;

        VkCommandBuffer trnCmdBuf;
        vulkanCore->createTransientCmdBuffer(trnCmdBuf);
        bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferCreateInfo.pNext = nullptr;
        bufferCreateInfo.size = bufferByteSize;
        bufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferCreateInfo.flags = 0;
        //err = vkCreateBuffer(m_device, &bufferCreateInfo, nullptr, &m_vertices.buf);
        err = vkCreateBuffer(device, sxr::BufferCreateInfo(bufferByteSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), nullptr, &(vertices->buf));
        SXR_VK_CHECK(!err);

        // Obtain the memory requirements for this buffer.
        VkMemoryRequirements mem_reqs;
        vkGetBufferMemoryRequirements(device, vertices->buf, &mem_reqs);
        SXR_VK_CHECK(!err);

        // And allocate memory according to those requirements.
        VkMemoryAllocateInfo memoryAllocateInfo = {};
        memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryAllocateInfo.pNext = nullptr;
        memoryAllocateInfo.allocationSize = 0;
        memoryAllocateInfo.memoryTypeIndex = 0;
        memoryAllocateInfo.allocationSize  = mem_reqs.size;
        pass = vulkanCore->GetMemoryTypeFromProperties(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memoryAllocateInfo.memoryTypeIndex);
        SXR_VK_CHECK(pass);

        VkDeviceMemory mem_staging_vert;
        VkBuffer buf_staging_vert;
        err = vkCreateBuffer(device, sxr::BufferCreateInfo(bufferByteSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), nullptr, &buf_staging_vert);
        SXR_VK_CHECK(!err);

        err = vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &mem_staging_vert);
        SXR_VK_CHECK(!err);

        // Now we need to map the memory of this new allocation so the CPU can edit it.
        void *data;

        err = vkMapMemory(device, mem_staging_vert, 0, memoryAllocateInfo.allocationSize, 0, &data);
        SXR_VK_CHECK(!err);

        // Copy our triangle verticies and colors into the mapped memory area.
        memcpy(data, mVertexData, bufferByteSize);

        // Unmap the memory back from the CPU.
        vkUnmapMemory(device, mem_staging_vert);
        err = vkBindBufferMemory(device, buf_staging_vert, mem_staging_vert, 0);
        SXR_VK_CHECK(!err);

        // Create Device memory optimal
        pass = vulkanCore->GetMemoryTypeFromProperties(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryAllocateInfo.memoryTypeIndex);
        SXR_VK_CHECK(pass);
        err = vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &vertices->mem);
        SXR_VK_CHECK(!err);
        // Bind our buffer to the memory.
        err = vkBindBufferMemory(device, vertices->buf, vertices->mem, 0);
        SXR_VK_CHECK(!err);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(trnCmdBuf, &beginInfo);
        VkBufferCopy copyRegion = {};
        copyRegion.srcOffset = 0; // Optional
        copyRegion.dstOffset = 0; // Optional
        copyRegion.size = bufferCreateInfo.size;
        vkCmdCopyBuffer(trnCmdBuf, buf_staging_vert, vertices->buf, 1, &copyRegion);
        vkEndCommandBuffer(trnCmdBuf);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &trnCmdBuf;

        vkQueueSubmit(vulkanCore->getVkQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(vulkanCore->getVkQueue());
        vkFreeCommandBuffers(device, vulkanCore->getTransientCmdPool(), 1, &trnCmdBuf);

        // Free up the staging buffer
        vkDestroyBuffer(device, buf_staging_vert, nullptr);
        vkFreeMemory(device, mem_staging_vert, nullptr);

        // The vertices need to be defined so that the pipeline understands how the
        // data is laid out. This is done by providing a VkPipelineVertexInputStateCreateInfo
        // structure with the correct information.
        vertices->vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertices->vi.pNext = nullptr;
        vertices->vi_bindings.stride = mTotalSize;
        vertices->vi.vertexAttributeDescriptionCount = i;
        vertices->vi.pVertexAttributeDescriptions = vertices->vi_attrs.data();
        vertices->vi.vertexBindingDescriptionCount = 1;
        vertices->vi.pVertexBindingDescriptions = &vertices->vi_bindings;
        vertices->vi_bindings.binding = 0;
        vertices->vi_bindings.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        mVerticesMap[shader] = vertices;
        mIsDirty = false;
    }

    VkFormat VulkanVertexBuffer::getDataType(const std::string& type)
    {
        if(type.compare("float")==0)
            return VK_FORMAT_R32_SFLOAT;

        if(type.compare("vec2")==0 || type.compare("float2")==0)
            return VK_FORMAT_R32G32_SFLOAT;

        if(type.compare("float3")==0 || type.compare("vec3")==0)
            return VK_FORMAT_R32G32B32_SFLOAT;

        if(type.compare("float4")==0 || type.compare("vec4")==0)
            return VK_FORMAT_R32G32B32A32_SFLOAT;

        if(type.compare("int4")==0 || type.compare("ivec4")==0)
            return VK_FORMAT_R32G32B32A32_SINT;

        FAIL("VulkanVertexBuffer::getDataType: unknown type %s", type.c_str());
        return VK_FORMAT_UNDEFINED;

    }
} // end sxrsdk
