/*
 * Copyright © 2019 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <vulkan/vulkan.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define MAX_ALLOWED_QUEUES 8
#define FENCE_TIMEOUT 1000000000ull /* 1s */

static VkResult
create_empty_cmdbuf(VkDevice device, VkCommandPool cmdpool,
                    VkCommandBuffer *cmdbuf)
{
    VkResult result;

    VkCommandBufferAllocateInfo allocateCmdbufInfo = {};
    allocateCmdbufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateCmdbufInfo.commandPool = cmdpool;
    allocateCmdbufInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateCmdbufInfo.commandBufferCount = 1;

    result = vkAllocateCommandBuffers(device, &allocateCmdbufInfo, cmdbuf);
    if (result != VK_SUCCESS)
        return result;

    VkCommandBufferBeginInfo beginCmdbufInfo = {};
    beginCmdbufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    result = vkBeginCommandBuffer(*cmdbuf, &beginCmdbufInfo);
    if (result != VK_SUCCESS)
        goto fail;

    result = vkEndCommandBuffer(*cmdbuf);
    if (result != VK_SUCCESS)
        goto fail;

    return result;
fail:
    vkFreeCommandBuffers(device, cmdpool, 1, cmdbuf);
    return result;
}

static unsigned
get_device_id()
{
    int device_id = 0;
    char *str;

    str = getenv("HANG_DETECTION_DEVICE_ID");
    if (str)
        device_id = strtol(str, NULL, 10);
    return device_id;
}

int main(int argc, char **argv)
{
    VkPhysicalDevice *physical_devices;
    uint32_t device_count;
    VkQueue queues[MAX_ALLOWED_QUEUES];
    uint32_t queue_count;
    VkInstance instance;
    VkResult result;
    int device_id;
    int ret = 0;
    int status;
    pid_t pid;

    device_id = get_device_id();

    pid = fork();
    if (!pid) {
        for (int i = 1; i < argc; i++) {
            argv[i - 1] = argv[i];
        }
        argv[argc - 1] = NULL;

        execvp(argv[0], argv);
    }

    /**
     * Instance creation.
     */
    VkInstanceCreateInfo instanceCreateInfo = {};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    result = vkCreateInstance(&instanceCreateInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create instance (%d).\n", result);
        goto fail_instance;
    }

    /**
     * Device creation.
     */
    /* Get number of devices. */
    result = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    fprintf(stderr, "Number of devices: %d\n", device_count);

    physical_devices = malloc(sizeof(*physical_devices) * device_count);

    /* Get physical devices. */
    result = vkEnumeratePhysicalDevices(instance, &device_count,
                                        physical_devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to enumerate physical devices (%d).\n", result);
        ret = -1;
        goto fail_device;
    }

    /* Check if the device ID is valid. */
    if (device_id > device_count - 1) {
        fprintf(stderr, "Invalid device ID found (device ID starts from 0)\n");
        ret = -1;
        goto fail_device;
    }

    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical_devices[device_id], &device_properties);
    fprintf(stderr, "GPU: %s\n", device_properties.deviceName);

    /* Get queue properties. */
    vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[device_id],
                                             &queue_count, NULL);
    assert(queue_count <= MAX_ALLOWED_QUEUES);

    /* Create logical device. */
    VkDeviceQueueCreateInfo queueCreateInfos[MAX_ALLOWED_QUEUES] = {};
    for (uint32_t q = 0; q < queue_count; q++) {
        queueCreateInfos[q].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfos[q].queueFamilyIndex = q;
        queueCreateInfos[q].queueCount = 1;
    }
    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = queue_count;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos;
    deviceCreateInfo.enabledExtensionCount = 0;
    deviceCreateInfo.ppEnabledExtensionNames = NULL;

    VkDevice device;
    result = vkCreateDevice(physical_devices[0], &deviceCreateInfo,
                            NULL, &device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create device (%d).\n", result);
        ret = -1;
        goto fail_device;
    }

    /* Get queues. */
    for (uint32_t q = 0; q < queue_count; q++)
        vkGetDeviceQueue(device, q, 0, &queues[q]);

    /* Create a command pool. */
    VkCommandPool cmdpool;
    VkCommandPoolCreateInfo createCmdpoolInfo = {};
    createCmdpoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;

    result = vkCreateCommandPool(device, &createCmdpoolInfo, NULL, &cmdpool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create a command buffer pool (%d).\n", result);
        ret = -1;
        goto fail_cmdpool;
    }

    /* Create an empty command buffer. */
    VkCommandBuffer cmdbuf;

    result = create_empty_cmdbuf(device, cmdpool, &cmdbuf);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create an empty command buffer (%d).\n", result);
        ret = -1;
        goto fail_cmdbuf;
    }

    /* Create a fence. */
    VkFence fence;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    result = vkCreateFence(device, &fenceInfo, NULL, &fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Failed to create a fence (%d).\n", result);
        ret = -1;
        goto fail_fence;
    }

    /* Submit an empty command buffer every 1s until the child process exits. */
    while (!waitpid(pid, &status, WNOHANG)) {
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdbuf;

        for (uint32_t q = 0; q < queue_count; q++) {
            /* Reset the fence and submit the empty command buffer. */
            vkResetFences(device, 1, &fence);
            vkQueueSubmit(queues[q], 1, &submitInfo, fence);

            /* Wait for the fence and kill the child process if a GPU hang is
             * detected.
             */
            result = vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT);
            if (result == VK_TIMEOUT) {
                fprintf(stderr, "GPU hang detected! Killing PID %d!\n", pid);
                kill(pid, SIGKILL);
                abort();
            }
        }

        sleep(1);
    }

fail_fence:
    vkFreeCommandBuffers(device, cmdpool, 1, &cmdbuf);
fail_cmdbuf:
    vkDestroyCommandPool(device, cmdpool, NULL);
fail_cmdpool:
    vkDestroyDevice(device, NULL);
fail_device:
    free(physical_devices);
    vkDestroyInstance(instance, NULL);
fail_instance:
    kill(pid, SIGKILL);

    return ret;
}
