/* WAYLANDDRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2021 Alexandros Frantzis
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <stdlib.h>

#include "waylanddrv.h"
#include "wine/debug.h"

#define VK_NO_PROTOTYPES
#define WINE_VK_HOST

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifdef SONAME_LIBVULKAN

#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000006000

typedef struct VkWaylandSurfaceCreateInfoKHR
{
    VkStructureType sType;
    const void *pNext;
    VkWaylandSurfaceCreateFlagsKHR flags;
    struct wl_display *display;
    struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR;

static VkResult (*pvkCreateInstance)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult (*pvkCreateWaylandSurfaceKHR)(VkInstance, const VkWaylandSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
static void (*pvkDestroyInstance)(VkInstance, const VkAllocationCallbacks *);
static void (*pvkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
static VkResult (*pvkEnumerateInstanceExtensionProperties)(const char *, uint32_t *, VkExtensionProperties *);
static void * (*pvkGetDeviceProcAddr)(VkDevice, const char *);
static void * (*pvkGetInstanceProcAddr)(VkInstance, const char *);
static VkResult (*pvkGetPhysicalDeviceSurfaceCapabilities2KHR)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, VkSurfaceCapabilities2KHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceFormats2KHR)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, uint32_t *, VkSurfaceFormat2KHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
static VkResult (*pvkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);

static void *vulkan_handle;
static const struct vulkan_funcs vulkan_funcs;

struct wine_vk_surface
{
    struct wayland_client_surface *client;
    VkSurfaceKHR native;
};

static struct wine_vk_surface *wine_vk_surface_from_handle(VkSurfaceKHR handle)
{
    return (struct wine_vk_surface *)(uintptr_t)handle;
}

static HWND wine_vk_surface_get_hwnd(struct wine_vk_surface *wine_vk_surface)
{
    return wl_surface_get_user_data(wine_vk_surface->client->wl_surface);
}

static void wine_vk_surface_destroy(struct wine_vk_surface *wine_vk_surface)
{
    if (wine_vk_surface->client)
    {
        HWND hwnd = wine_vk_surface_get_hwnd(wine_vk_surface);
        struct wayland_surface *wayland_surface = wayland_surface_lock_hwnd(hwnd);

        if (wayland_client_surface_release(wine_vk_surface->client) &&
            wayland_surface)
        {
            wayland_surface->client = NULL;
        }

        if (wayland_surface) pthread_mutex_unlock(&wayland_surface->mutex);
    }

    free(wine_vk_surface);
}

static BOOL wine_vk_surface_is_valid(struct wine_vk_surface *wine_vk_surface)
{
    HWND hwnd = wine_vk_surface_get_hwnd(wine_vk_surface);
    struct wayland_surface *wayland_surface;

    if ((wayland_surface = wayland_surface_lock_hwnd(hwnd)))
    {
        pthread_mutex_unlock(&wayland_surface->mutex);
        return TRUE;
    }

    return FALSE;
}

/* Helper function for converting between win32 and Wayland compatible VkInstanceCreateInfo.
 * Caller is responsible for allocation and cleanup of 'dst'. */
static VkResult wine_vk_instance_convert_create_info(const VkInstanceCreateInfo *src,
                                                     VkInstanceCreateInfo *dst)
{
    unsigned int i;
    const char **enabled_extensions = NULL;

    dst->sType = src->sType;
    dst->flags = src->flags;
    dst->pApplicationInfo = src->pApplicationInfo;
    dst->pNext = src->pNext;
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;
    dst->enabledExtensionCount = 0;
    dst->ppEnabledExtensionNames = NULL;

    if (src->enabledExtensionCount > 0)
    {
        enabled_extensions = calloc(src->enabledExtensionCount,
                                    sizeof(*src->ppEnabledExtensionNames));
        if (!enabled_extensions)
        {
            ERR("Failed to allocate memory for enabled extensions\n");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        for (i = 0; i < src->enabledExtensionCount; i++)
        {
            /* Substitute extension with Wayland ones else copy. Long-term, when we
             * support more extensions, we should store these in a list. */
            if (!strcmp(src->ppEnabledExtensionNames[i], "VK_KHR_win32_surface"))
                enabled_extensions[i] = "VK_KHR_wayland_surface";
            else
                enabled_extensions[i] = src->ppEnabledExtensionNames[i];
        }
        dst->ppEnabledExtensionNames = enabled_extensions;
        dst->enabledExtensionCount = src->enabledExtensionCount;
    }

    return VK_SUCCESS;
}

static const char *wine_vk_native_fn_name(const char *name)
{
    if (!strcmp(name, "vkCreateWin32SurfaceKHR"))
        return "vkCreateWaylandSurfaceKHR";

    return name;
}

/* Update the capabilities to match what the Win32 WSI would provide. */
static VkResult wine_vk_surface_update_caps(struct wine_vk_surface *wine_vk_surface,
                                            VkSurfaceCapabilitiesKHR *caps)
{
    struct wayland_surface *wayland_surface;
    int client_width, client_height;
    HWND hwnd = wine_vk_surface_get_hwnd(wine_vk_surface);

    if (!(wayland_surface = wayland_surface_lock_hwnd(hwnd)))
        return VK_ERROR_SURFACE_LOST_KHR;

    client_width = wayland_surface->window.client_rect.right -
                   wayland_surface->window.client_rect.left;
    client_height = wayland_surface->window.client_rect.bottom -
                    wayland_surface->window.client_rect.top;

    pthread_mutex_unlock(&wayland_surface->mutex);

    caps->minImageExtent.width = client_width;
    caps->minImageExtent.height = client_height;
    caps->maxImageExtent.width = client_width;
    caps->maxImageExtent.height = client_height;
    caps->currentExtent.width = client_width;
    caps->currentExtent.height = client_height;

    TRACE("hwnd=%p extent=%dx%d\n", hwnd,
          caps->currentExtent.width, caps->currentExtent.height);

    return VK_SUCCESS;
}

static VkResult wayland_vkCreateInstance(const VkInstanceCreateInfo *create_info,
                                         const VkAllocationCallbacks *allocator,
                                         VkInstance *instance)
{
    VkInstanceCreateInfo create_info_host;
    VkResult res;

    TRACE("create_info %p, allocator %p, instance %p\n", create_info, allocator, instance);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* Perform a second pass on converting VkInstanceCreateInfo. Winevulkan
     * performed a first pass in which it handles everything except for WSI
     * functionality such as VK_KHR_win32_surface. Handle this now. */
    res = wine_vk_instance_convert_create_info(create_info, &create_info_host);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to convert instance create info, res=%d\n", res);
        return res;
    }

    res = pvkCreateInstance(&create_info_host, NULL /* allocator */, instance);

    free((void *)create_info_host.ppEnabledExtensionNames);
    return res;
}

static VkResult wayland_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                const VkWin32SurfaceCreateInfoKHR *create_info,
                                                const VkAllocationCallbacks *allocator,
                                                VkSurfaceKHR *vk_surface)
{
    VkResult res;
    VkWaylandSurfaceCreateInfoKHR create_info_host;
    struct wine_vk_surface *wine_vk_surface;
    struct wayland_surface *wayland_surface;

    TRACE("%p %p %p %p\n", instance, create_info, allocator, vk_surface);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    wine_vk_surface = calloc(1, sizeof(*wine_vk_surface));
    if (!wine_vk_surface)
    {
        ERR("Failed to allocate memory for wayland vulkan surface\n");
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }

    wayland_surface = wayland_surface_lock_hwnd(create_info->hwnd);
    if (!wayland_surface)
    {
        ERR("Failed to find wayland surface for hwnd=%p\n", create_info->hwnd);
        /* VK_KHR_win32_surface only allows out of host and device memory as errors. */
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }

    wine_vk_surface->client = wayland_surface_get_client(wayland_surface);
    pthread_mutex_unlock(&wayland_surface->mutex);

    if (!wine_vk_surface->client)
    {
        ERR("Failed to create client surface for hwnd=%p\n", create_info->hwnd);
        /* VK_KHR_win32_surface only allows out of host and device memory as errors. */
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto err;
    }

    create_info_host.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.display = process_wayland.wl_display;
    create_info_host.surface = wine_vk_surface->client->wl_surface;

    res = pvkCreateWaylandSurfaceKHR(instance, &create_info_host,
                                     NULL /* allocator */,
                                     &wine_vk_surface->native);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create vulkan wayland surface, res=%d\n", res);
        goto err;
    }

    *vk_surface = (uintptr_t)wine_vk_surface;

    TRACE("Created surface=0x%s\n", wine_dbgstr_longlong(*vk_surface));
    return VK_SUCCESS;

err:
    if (wine_vk_surface) wine_vk_surface_destroy(wine_vk_surface);
    return res;
}

static void wayland_vkDestroyInstance(VkInstance instance,
                                      const VkAllocationCallbacks *allocator)
{
    TRACE("%p %p\n", instance, allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    pvkDestroyInstance(instance, NULL /* allocator */);
}

static void wayland_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                        const VkAllocationCallbacks *allocator)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);

    TRACE("%p 0x%s %p\n", instance, wine_dbgstr_longlong(surface), allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* vkDestroySurfaceKHR must handle VK_NULL_HANDLE (0) for surface. */
    if (!wine_vk_surface) return;

    pvkDestroySurfaceKHR(instance, wine_vk_surface->native, NULL /* allocator */);
    wine_vk_surface_destroy(wine_vk_surface);
}

static VkResult wayland_vkEnumerateInstanceExtensionProperties(const char *layer_name,
                                                               uint32_t *count,
                                                               VkExtensionProperties* properties)
{
    unsigned int i;
    VkResult res;

    TRACE("layer_name %s, count %p, properties %p\n", debugstr_a(layer_name), count, properties);

    /* This shouldn't get called with layer_name set, the ICD loader prevents it. */
    if (layer_name)
    {
        ERR("Layer enumeration not supported from ICD.\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    /* We will return the same number of instance extensions reported by the host back to
     * winevulkan. Along the way we may replace xlib extensions with their win32 equivalents.
     * Winevulkan will perform more detailed filtering as it knows whether it has thunks
     * for a particular extension.
     */
    res = pvkEnumerateInstanceExtensionProperties(layer_name, count, properties);
    if (!properties || res < 0)
        return res;

    for (i = 0; i < *count; i++)
    {
        /* For now the only wayland extension we need to fixup. Long-term we may need an array. */
        if (!strcmp(properties[i].extensionName, "VK_KHR_wayland_surface"))
        {
            TRACE("Substituting VK_KHR_wayland_surface for VK_KHR_win32_surface\n");

            snprintf(properties[i].extensionName, sizeof(properties[i].extensionName),
                    VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
            properties[i].specVersion = VK_KHR_WIN32_SURFACE_SPEC_VERSION;
        }
    }

    TRACE("Returning %u extensions.\n", *count);
    return res;
}

static void *wayland_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    void *proc_addr;

    TRACE("%p, %s\n", device, debugstr_a(name));

    /* Do not return the driver function if the corresponding native function
     * is not available. */
    if (!pvkGetDeviceProcAddr(device, wine_vk_native_fn_name(name)))
        return NULL;

    if ((proc_addr = get_vulkan_driver_device_proc_addr(&vulkan_funcs, name)))
        return proc_addr;

    return pvkGetDeviceProcAddr(device, name);
}

static void *wayland_vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    void *proc_addr;

    TRACE("%p, %s\n", instance, debugstr_a(name));

    /* Do not return the driver function if the corresponding native function
     * is not available. */
    if (!pvkGetInstanceProcAddr(instance, wine_vk_native_fn_name(name)))
        return NULL;

    if ((proc_addr = get_vulkan_driver_instance_proc_addr(&vulkan_funcs, instance, name)))
        return proc_addr;

    return pvkGetInstanceProcAddr(instance, name);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice phys_dev,
                                                                   const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                   VkSurfaceCapabilities2KHR *capabilities)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface_info->surface);
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host;
    VkResult res;

    TRACE("%p, %p, %p\n", phys_dev, surface_info, capabilities);

    surface_info_host = *surface_info;
    surface_info_host.surface = wine_vk_surface->native;

    if (pvkGetPhysicalDeviceSurfaceCapabilities2KHR)
    {
        res = pvkGetPhysicalDeviceSurfaceCapabilities2KHR(phys_dev, &surface_info_host,
                                                          capabilities);
        goto out;
    }

    /* Until the loader version exporting this function is common, emulate it
     * using the older non-2 version. */
    if (surface_info->pNext || capabilities->pNext)
    {
        FIXME("Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR with "
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR, pNext is ignored.\n");
    }

    res = pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface_info_host.surface,
                                                     &capabilities->surfaceCapabilities);

out:
    if (res == VK_SUCCESS)
        res = wine_vk_surface_update_caps(wine_vk_surface, &capabilities->surfaceCapabilities);

    return res;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice phys_dev,
                                                                  VkSurfaceKHR surface,
                                                                  VkSurfaceCapabilitiesKHR *capabilities)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);
    VkResult res;

    TRACE("%p, 0x%s, %p\n", phys_dev, wine_dbgstr_longlong(surface), capabilities);

    res = pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, wine_vk_surface->native,
                                                     capabilities);
    if (res == VK_SUCCESS)
        res = wine_vk_surface_update_caps(wine_vk_surface, capabilities);

    return res;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice phys_dev,
                                                              const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                              uint32_t *count,
                                                              VkSurfaceFormat2KHR *formats)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface_info->surface);
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host;
    VkSurfaceFormatKHR *formats_host;
    uint32_t i;
    VkResult result;

    TRACE("%p, %p, %p, %p\n", phys_dev, surface_info, count, formats);

    if (!wine_vk_surface_is_valid(wine_vk_surface))
        return VK_ERROR_SURFACE_LOST_KHR;

    surface_info_host = *surface_info;
    surface_info_host.surface = wine_vk_surface->native;

    if (pvkGetPhysicalDeviceSurfaceFormats2KHR)
    {
        return pvkGetPhysicalDeviceSurfaceFormats2KHR(phys_dev, &surface_info_host,
                                                      count, formats);
    }

    /* Until the loader version exporting this function is common, emulate it
     * using the older non-2 version. */
    if (surface_info->pNext)
    {
        FIXME("Emulating vkGetPhysicalDeviceSurfaceFormats2KHR with "
              "vkGetPhysicalDeviceSurfaceFormatsKHR, pNext is ignored.\n");
    }

    if (!formats)
    {
        return pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_info_host.surface,
                                                     count, NULL);
    }

    formats_host = calloc(*count, sizeof(*formats_host));
    if (!formats_host) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_info_host.surface,
                                                   count, formats_host);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE)
    {
        for (i = 0; i < *count; i++)
            formats[i].surfaceFormat = formats_host[i];
    }

    free(formats_host);
    return result;
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice phys_dev,
                                                             VkSurfaceKHR surface,
                                                             uint32_t *count,
                                                             VkSurfaceFormatKHR *formats)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);

    TRACE("%p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, formats);

    if (!wine_vk_surface_is_valid(wine_vk_surface))
        return VK_ERROR_SURFACE_LOST_KHR;

    return pvkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, wine_vk_surface->native,
                                                 count, formats);
}

static VkResult wayland_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice phys_dev,
                                                             uint32_t index,
                                                             VkSurfaceKHR surface,
                                                             VkBool32 *supported)
{
    struct wine_vk_surface *wine_vk_surface = wine_vk_surface_from_handle(surface);

    TRACE("%p, %u, 0x%s, %p\n", phys_dev, index, wine_dbgstr_longlong(surface), supported);

    if (!wine_vk_surface_is_valid(wine_vk_surface))
        return VK_ERROR_SURFACE_LOST_KHR;

    return pvkGetPhysicalDeviceSurfaceSupportKHR(phys_dev, index, wine_vk_surface->native,
                                                 supported);
}

static VkSurfaceKHR wayland_wine_get_native_surface(VkSurfaceKHR surface)
{
    return wine_vk_surface_from_handle(surface)->native;
}

static void wine_vk_init(void)
{
    if (!(vulkan_handle = dlopen(SONAME_LIBVULKAN, RTLD_NOW)))
    {
        ERR("Failed to load %s.\n", SONAME_LIBVULKAN);
        return;
    }

#define LOAD_FUNCPTR(f) if (!(p##f = dlsym(vulkan_handle, #f))) goto fail
#define LOAD_OPTIONAL_FUNCPTR(f) p##f = dlsym(vulkan_handle, #f)
    LOAD_FUNCPTR(vkCreateInstance);
    LOAD_FUNCPTR(vkCreateWaylandSurfaceKHR);
    LOAD_FUNCPTR(vkDestroyInstance);
    LOAD_FUNCPTR(vkDestroySurfaceKHR);
    LOAD_FUNCPTR(vkEnumerateInstanceExtensionProperties);
    LOAD_FUNCPTR(vkGetDeviceProcAddr);
    LOAD_FUNCPTR(vkGetInstanceProcAddr);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_OPTIONAL_FUNCPTR(vkGetPhysicalDeviceSurfaceFormats2KHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_FUNCPTR(vkGetPhysicalDeviceSurfaceSupportKHR);
#undef LOAD_FUNCPTR
#undef LOAD_OPTIONAL_FUNCPTR

    return;

fail:
    dlclose(vulkan_handle);
    vulkan_handle = NULL;
}

static const struct vulkan_funcs vulkan_funcs =
{
    .p_vkCreateInstance = wayland_vkCreateInstance,
    .p_vkCreateWin32SurfaceKHR = wayland_vkCreateWin32SurfaceKHR,
    .p_vkDestroyInstance = wayland_vkDestroyInstance,
    .p_vkDestroySurfaceKHR = wayland_vkDestroySurfaceKHR,
    .p_vkEnumerateInstanceExtensionProperties = wayland_vkEnumerateInstanceExtensionProperties,
    .p_vkGetDeviceProcAddr = wayland_vkGetDeviceProcAddr,
    .p_vkGetInstanceProcAddr = wayland_vkGetInstanceProcAddr,
    .p_vkGetPhysicalDeviceSurfaceCapabilities2KHR = wayland_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = wayland_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    .p_vkGetPhysicalDeviceSurfaceFormats2KHR = wayland_vkGetPhysicalDeviceSurfaceFormats2KHR,
    .p_vkGetPhysicalDeviceSurfaceFormatsKHR = wayland_vkGetPhysicalDeviceSurfaceFormatsKHR,
    .p_vkGetPhysicalDeviceSurfaceSupportKHR = wayland_vkGetPhysicalDeviceSurfaceSupportKHR,
    .p_wine_get_native_surface = wayland_wine_get_native_surface,
};

/**********************************************************************
 *           WAYLAND_wine_get_vulkan_driver
 */
const struct vulkan_funcs *WAYLAND_wine_get_vulkan_driver(UINT version)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, vulkan wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return NULL;
    }

    pthread_once(&init_once, wine_vk_init);
    if (vulkan_handle)
        return &vulkan_funcs;

    return NULL;
}

#else /* No vulkan */

const struct vulkan_funcs *WAYLAND_wine_get_vulkan_driver(UINT version)
{
    ERR("Wine was built without Vulkan support.\n");
    return NULL;
}

#endif /* SONAME_LIBVULKAN */