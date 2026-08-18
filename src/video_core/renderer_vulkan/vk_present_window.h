// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

class Instance;
class Swapchain;
class Scheduler;
class RenderManager;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Framebuffer framebuffer;
    vk::Image image;
    vk::ImageView image_view;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    vk::CommandBuffer cmdbuf;
};

class PresentWindow final {
public:
    explicit PresentWindow(Frontend::EmuWindow& emu_window, const Instance& instance,
                           Scheduler& scheduler, bool low_refresh_rate);
    ~PresentWindow();

    /// Waits for all queued frames to finish presenting.
    void WaitPresent();

    /// Returns the last used render frame.
    Frame* GetRenderFrame();

    /// Recreates the render frame to match provided parameters.
    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// Queues the provided frame for presentation.
    void Present(Frame* frame);

    /// This is called to notify the rendering backend of a surface change
    void NotifySurfaceChanged();

    [[nodiscard]] vk::RenderPass Renderpass() const noexcept {
        return present_renderpass;
    }

    u32 ImageCount() const noexcept {
        return static_cast<u32>(swap_chain.size());
    }

    vk::Format GetSurfaceFormat() const noexcept {
        return output_format;
    }

    bool CaptureRGBA(std::vector<u8>& out);

private:
    void PresentThread(std::stop_token token);

    void CopyToPresentTarget(Frame* frame);

    void CreateOffscreenTarget(u32 width, u32 height);

    void DestroyOffscreenTarget();

    vk::RenderPass CreateRenderpass();

private:
    Frontend::EmuWindow& emu_window;
    const Instance& instance;
    Scheduler& scheduler;
    bool low_refresh_rate;
    bool use_offscreen_target;
    vk::SurfaceKHR surface;
    vk::SurfaceKHR next_surface{};
    std::unique_ptr<Swapchain> swapchain;
    vk::CommandPool command_pool;
    vk::Queue graphics_queue;
    vk::RenderPass present_renderpass;
    std::vector<Frame> swap_chain;
    std::queue<Frame*> free_queue;
    std::queue<Frame*> present_queue;
    std::condition_variable free_cv;
    std::condition_variable recreate_surface_cv;
    std::condition_variable_any frame_cv;
    std::mutex swapchain_mutex;
    std::mutex recreate_surface_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    bool vsync_enabled{};
    bool blit_supported;
    bool use_present_thread{true};
    void* last_render_surface{};
    vk::Image output_image{};
    vk::ImageView output_image_view{};
    VmaAllocation output_allocation{};
    vk::Format output_format{vk::Format::eR8G8B8A8Unorm};
    u32 output_width{};
    u32 output_height{};
    bool output_image_initialized{};
};

} // namespace Vulkan
