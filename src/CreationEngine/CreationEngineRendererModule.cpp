//
// Created by sergp on 6/26/2024.
//

#include "PerfStats.h"
#include "CreationEngineRendererModule.h"
#include "CreationEngineCameraManager.h"
#include "CreationEngineConstants.h"
#include "CreationEngineSingletonManager.h"
#include "REL/Relocation.h"
#include <CreationEngine/memory/offsets.h>
#include <CreationEngine/models/GameFlow.h>
#include <CreationEngine/models/ModSettingsStore.h>
#include <_deps/directxtk12-src/Src/d3dx12.h>
#include <mods/VR.hpp>
#include <safetyhook/easy.hpp>

#include "ModSettings.h"

uintptr_t onUpdateConstantBufferViewDetour(uint8_t copyPresentToPast, uint8_t orphoProjection, uintptr_t pCameraTransforms, uintptr_t vararg1Parent, uintptr_t vararg3Parent,
                                           double unk, char unk2, RE::RenderPassConstantBufferView* camerablocks)
{
    return CreationEngineRendererModule::Get()->onUpdateConstantBufferView(copyPresentToPast, orphoProjection, pCameraTransforms, vararg1Parent, vararg3Parent, unk, unk2,
                                                                           camerablocks);
}

__int64 onRenderGraphRenderStartDetour(RE::CreationRendererPrivate::RenderGraph* rcx, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData, __int64 r8, __int64 r9)
{
    return CreationEngineRendererModule::Get()->onRenderGraphRenderStart(rcx, pRenderGraphData, r8, r9);
}

//__int64 onRenderFrameStartDetour(void* rcx, __int64 rdx, __int64 r8, __int64 r9)
//{
//    return CreationEngineRendererModule::Get()->onRenderFrameStart(rcx, rdx, r8, r9);
//}

std::unique_ptr<FunctionHook> m_onWindowMessageHook;

LRESULT CALLBACK WndProcDetour(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    using func_t              = decltype(WndProcDetour);
    static auto original_func = m_onWindowMessageHook->get_original<func_t>();
    //    spdlog::info("Window message hit {}", message);
    return original_func(hWnd, message, wParam, lParam);
}

void CreationEngineRendererModule::InstallHooks()
{
//    // 0x25dafe4 - this one works but sometimes give 2 ticks
//    REL::Relocation<uintptr_t> onWorldTickFn{ (uintptr_t )mod + 0x25e267c };
//    m_worldTick_hook = safetyhook::create_inline((void*)onWorldTickFn.address(), (void*)worldTick);
////        std::make_unique<FunctionHook>(onWorldTickFn.address(), reinterpret_cast<uint64_t>(&worldTick));
//    if(!m_worldTick_hook) {
//        spdlog::error("Failed to create hook for onWorldTick");
//    }

    REL::Relocation<uintptr_t> setReflexMarkerInternalFn{ GameStore::MemoryOffsets::Nvidia::onSetReflexMarkerInternal() };
    m_setReflexMarkerInternalHook = safetyhook::create_inline((void*)setReflexMarkerInternalFn.address(), (void*)setReflexMarkerInternal);
    if(!m_setReflexMarkerInternalHook) {
        spdlog::error("Failed to create hook for setReflexMarkerInternal");
    }

    //    REL::Relocation<RE::CreationEngineSettings**> settings{ REL::ID(878340) };
    REL::Relocation<RE::CreationEngineSettings**> settings{ GameStore::MemoryOffsets::GlobalRenderSettings() };
    m_creationEngineSettings = settings.get();

    REL::Relocation<uintptr_t> onUpdateConstantBufferViewAddr{ GameStore::MemoryOffsets::CreationRenderer::OnUpdateConstantBufferView() };
    m_onUpdateConstantBufferViewHook = std::make_unique<FunctionHook>(onUpdateConstantBufferViewAddr.address(), reinterpret_cast<uint64_t>(&onUpdateConstantBufferViewDetour));
    m_onUpdateConstantBufferViewHook->create();

    //    REL::Relocation<uintptr_t> onRenderGraphRenderStartFuncAddr{ REL::ID(1079045) };
    REL::Relocation<uintptr_t> onRenderGraphRenderStartFuncAddr{ GameStore::MemoryOffsets::CreationRenderer::RenderGraphFrameStart() };
    m_onRenderGraphRenderStartHook = std::make_unique<FunctionHook>(onRenderGraphRenderStartFuncAddr.address(), reinterpret_cast<uint64_t>(&onRenderGraphRenderStartDetour));
    m_onRenderGraphRenderStartHook->create();

    // ID is exactly incrementing frames 149000
    // 202136 ID is function on start is frame start on end is frame end, however it is significantly decreases fps
    //    REL::Relocation<uintptr_t> onRenderFrameStartFuncAddr{ REL::ID(202136) };
//    REL::Relocation<uintptr_t> onRenderFrameStartFuncAddr{ GameStore::MemoryOffsets::CreationRenderer::RenderGraphRenderPipelineExecute() };
//    m_onRenderFrameStartHook = std::make_unique<FunctionHook>(onRenderFrameStartFuncAddr.address(), reinterpret_cast<uint64_t>(&onRenderFrameStartDetour));
//    m_onRenderFrameStartHook->create();

    REL::Relocation<uintptr_t> taa_vfunc7_hook_addr{ GameStore::MemoryOffsets::CreationRenderer::OnTaaVFunc7() };
    taa_vfunc7_hook = std::make_unique<FunctionHook>(taa_vfunc7_hook_addr.address(), reinterpret_cast<uint64_t>(&CreationEngineRendererModule::onTaaPass));
    taa_vfunc7_hook->create();
}

struct CameraBlockSnapshot
{
    Matrix4x4f cameraMatrix;
    Vector2f   cameraPositionOrJitter;
    int        frameNumber;
};

std::unordered_map<uintptr_t, CameraBlockSnapshot> pastProjections{};

__int64 CreationEngineRendererModule::onRenderGraphRenderStart(RE::CreationRendererPrivate::RenderGraph* pGraph, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData,
                                                               __int64 i1, __int64 i2)
{
    using func_t              = decltype(onRenderGraphRenderStartDetour);
    static auto original_func = m_onRenderGraphRenderStartHook->get_original<func_t>();
    RenderGraphStart(pGraph, pRenderGraphData, true);
    auto result = original_func(pGraph, pRenderGraphData, i1, i2);
    RenderGraphStart(pGraph, pRenderGraphData, false);
    return result;
}

bool CreationEngineRendererModule::ValidateResource(ID3D12Resource* source, ComPtr<ID3D12Resource> pastBuffer[4])
{
    if (source == nullptr) {
        return false;
    }
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    if (pastBuffer[0] != nullptr) {
        D3D12_RESOURCE_DESC desc2 = pastBuffer[0]->GetDesc();
        if (desc.Width != desc2.Width || desc.Height != desc2.Height || desc.Format != desc2.Format) {
            spdlog::info("Resource size mismatch {} {} {} {} {} {} {} {}", fmt::ptr(source), desc.Width, desc.Height, desc.Format, fmt::ptr(pastBuffer[0].Get()), desc2.Width,
                         desc2.Height, desc2.Format);
            pastBuffer[0].Reset();
            pastBuffer[1].Reset();
            pastBuffer[2].Reset();
            pastBuffer[3].Reset();
        }
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    if (pastBuffer[0] == nullptr || pastBuffer[1] == nullptr) {
        D3D12_HEAP_PROPERTIES heap_props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for (int i = 0; i < 4; i++) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(pastBuffer[i].GetAddressOf()))))
            {
                spdlog::error("[VR] Failed to create resource copy copy for AFR backbuffer {}.", i);
                return false;
            }
        }
    }
    return true;
}

void CreationEngineRendererModule::SwapBuffer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* originalBuffer, int index, int copyFromSlot, int copyToSlot)
{
    if (!ValidateResource(originalBuffer, m_pastBuffer[index])) {
        return;
    }
    auto saveBuffer       = m_pastBuffer[index][copyToSlot].Get();
    auto alterFrameBuffer = m_pastBuffer[index][copyFromSlot].Get();
    CopyResource(cmdList, originalBuffer, saveBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
    CopyResource(cmdList, alterFrameBuffer, originalBuffer, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void CreationEngineRendererModule::RenderGraphStart(RE::CreationRendererPrivate::RenderGraph* pGraph, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData, bool before)
{
    if (m_startFramePass == nullptr && strcmp(pGraph->name, "CRBeginFrame") == 0) {
        m_startFramePass = pGraph;
    }
    if (m_endFramePass == nullptr && strcmp(pGraph->name, "CREndFrame") == 0) {
        m_endFramePass = pGraph;
    }
    if (m_framePass == nullptr && strcmp(pGraph->name, "Frame") == 0) {
        m_framePass = pGraph;
    }
    auto vr = VR::get();
    if (m_startFramePass == pGraph && before) {
        GameFlow::resetGameState();
        ModSettings::g_internalSettings.showQuadDisplay = GameFlow::isShowingMenu();
    } else if (!before && m_framePass == pGraph) {
        auto fc          = GameFlow::renderLoopFrameCount();
        auto context     = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(pRenderGraphData->getCommandList());
        auto commandList = context->pID3D12CommandList;
        if (vr->is_hmd_active() && GameFlow::gStore.internalSettings.nvidiaAndTAAfix && ModConstants::enabledResourcesToCopy[1]) {
            auto resource = pRenderGraphData->getResourceByIndex(1, (fc - 1) & 1);
            SwapBuffer(commandList, resource, 1, (fc - 1) & 1, fc & 1);
        }
    }
}

//__int64 CreationEngineRendererModule::onRenderFrameStart(void* pVoid, __int64 i, __int64 i1, __int64 i2)
//{
//    using func_t              = decltype(onRenderFrameStartDetour);
//    static auto original_func = m_onRenderFrameStartHook->get_original<func_t>();
////    spdlog::info("Frame Submit Start[{:X}], fc=[m={},r={}]", GetCurrentThreadId(), GameFlow::gameLoopFrameCount(), GameFlow::renderLoopFrameCount());
//    auto result = original_func(pVoid, i, i1, i2);
//    return result;
//}


void CreationEngineRendererModule::SetWindowSize(int width, int height)
{
    static std::atomic<bool> inside_change{ false };
    static int               last_synced_frame{ 1000 };
    auto                     fc = GameFlow::renderLoopFrameCount();
    if(inside_change.exchange(true)) {
        return;
    }

    if (m_creationEngineSettings == nullptr || *m_creationEngineSettings == nullptr || (fc - last_synced_frame) < 5*72) {
        inside_change.store(false);
        return;
    }
    last_synced_frame = fc;
    inside_change.store(false);

    if (width == 0 || height == 0) {
        auto vr = VR::get();
        if (!vr->is_hmd_active()) {
            return;
        }
        width   = vr->get_hmd_width();
        height  = vr->get_hmd_height();
        if(width == 0 || height == 0) {
            return;
        }
    }

    // Compare the render target, not the window: Windows clamps bordered windows to the screen size.
    constexpr int kMaxResizeAttempts = 3;
    static int    resize_attempts{ 0 };
    static int    last_target_width{ 0 };
    static int    last_target_height{ 0 };

    const auto backbuffer = VR::get()->get_backbuffer_size();
    if ((int)backbuffer[0] == width && (int)backbuffer[1] == height) {
        resize_attempts = 0;
        return;
    }
    if (width != last_target_width || height != last_target_height) {
        last_target_width  = width;
        last_target_height = height;
        resize_attempts    = 0;
    }
    if (resize_attempts >= kMaxResizeAttempts) {
        if (resize_attempts++ == kMaxResizeAttempts) {
            spdlog::warn("Render size is still {}x{} after {} resize requests for {}x{}; giving up", backbuffer[0], backbuffer[1], kMaxResizeAttempts, width, height);
        }
        return;
    }
    ++resize_attempts;

    spdlog::info("Setting window size to {} {} (back buffer {}x{}, attempt {})", width, height, backbuffer[0], backbuffer[1], resize_attempts);
    auto ce_rect = &(*m_creationEngineSettings)->displayGameSettings.displayRect;

    ce_rect->cx = width + ce_rect->x;
    ce_rect->cy = height + ce_rect->y;
    (*m_creationEngineSettings)->displayGameSettings.flags |= 0x100;
//    WINDOWPOS pos;
//    pos.hwnd            = (*m_creationEngineSettings)->pHwindow->windowHandle;
//    pos.hwndInsertAfter = nullptr;
//    pos.x               = rect->x;
//    pos.y               = rect->y;
//    pos.cx              = width;
//    pos.cy              = height;
//    pos.flags           = 0;
//    SendMessage((*m_creationEngineSettings)->pHwindow->windowHandle, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&pos));
    auto  hWnd      = g_framework->get_window();
    RECT  rect      = { 0, 0, (LONG)width, (LONG)height };
    DWORD dwStyle   = GetWindowLong(hWnd, GWL_STYLE);
    DWORD dwExStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
    BOOL  hasMenu   = GetMenu(hWnd) != NULL;
    AdjustWindowRectEx(&rect, dwStyle, hasMenu, dwExStyle);

    int nWidth  = rect.right - rect.left;
    int nHeight = rect.bottom - rect.top;
    spdlog::info("Setting window size to {} {}", nWidth, nHeight);
    SetWindowPos(hWnd, nullptr, 0, 0, nWidth, nHeight, SWP_ASYNCWINDOWPOS);
}

uintptr_t CreationEngineRendererModule::onUpdateConstantBufferView(uint8_t copyCurrentToPast, uint8_t resetHistory, uintptr_t i2, uintptr_t i3, uintptr_t i4, double d, char i5,
                                                                   RE::RenderPassConstantBufferView* pView)
{
    using func_t              = decltype(onUpdateConstantBufferViewDetour);
    static auto original_func = m_onUpdateConstantBufferViewHook->get_original<func_t>();
    static auto vr            = VR::get();
    auto        result        = original_func(copyCurrentToPast, resetHistory, i2, i3, i4, d, i5, pView);
    //    spdlog::info("update camera blocks {} index {} handles[{},{},{}] st ptr {}", fmt::ptr(pView), indexAfterPresent, cameraHandleId, cameraHandleId2, cameraHandleId3,
    //    fmt::ptr(cameraBlocks->data));
    if (copyCurrentToPast && vr->is_hmd_active() && GameFlow::gStore.internalSettings.nvidiaAndTAAfix) {
        auto key = reinterpret_cast<uintptr_t>(pView) & 0xFFFFFFFFFFFFFFC0;
        if (pastProjections.find(key) != pastProjections.end()) {
            auto temp                         = pastProjections[key];
            auto past2past                    = &pastProjections[key];
            past2past->cameraMatrix           = pView->unk150;
            past2past->cameraPositionOrJitter = pView->unk190;
            past2past->frameNumber            = pView->unk198;
            if (!resetHistory) {
                pView->unk150 = temp.cameraMatrix;
                pView->unk190 = temp.cameraPositionOrJitter;
                pView->unk198 = temp.frameNumber;
            }
        }
        else {
            CameraBlockSnapshot past{};
            past.cameraMatrix           = pView->unk150;
            past.cameraPositionOrJitter = pView->unk190;
            past.frameNumber            = pView->unk198;
            pastProjections[key]        = past;
        }
    }
    return result;
}

uintptr_t CreationEngineRendererModule::onTaaPass(RE::CreationRendererPrivate::RenderPass* pPass, RE::CreationRendererPrivate::RenderGraphData* pRenderGraphData,
                                                  RE::CreationRendererPrivate::RenderPassData* renderPassData)
{
    static auto instance    = Get();
    static auto original_fn = instance->taa_vfunc7_hook->get_original<decltype(CreationEngineRendererModule::onTaaPass)>();
    auto        result      = original_fn(pPass, pRenderGraphData, renderPassData);
    if(!GameFlow::gStore.internalSettings.nvidiaAndTAAfix) {
        return result;
    }
    auto        fc          = GameFlow::renderLoopFrameCount();
    auto        context     = reinterpret_cast<RE::RenderGraphDataD3D12Context*>(pRenderGraphData->getCommandList());
    auto        commandList = context->pID3D12CommandList;
    static auto vr          = VR::get();

    if (vr->is_hmd_active() && ModConstants::enabledResourcesToCopy[2]) {
        auto resource = pRenderGraphData->getResourceByIndex(2, (fc - 1) & 1);
        instance->SwapBuffer(commandList, resource, 2, (fc - 1) & 1, fc & 1);
    }
    if (vr->is_hmd_active() && ModConstants::enabledResourcesToCopy[3]) {
        auto resource = pRenderGraphData->getResourceByIndex(3, (fc - 1) & 1);
        instance->SwapBuffer(commandList, resource, 3, (fc - 1) & 1, fc & 1);
    }
    return result;
}

/*

void logWithExtraData(std::string_view message) {
    spdlog::info("[{:X}:{}] frameCount[m={},r={}]", GetCurrentThreadId(), message.data(), GameFlow::gameLoopFrameCount(), GameFlow::renderLoopFrameCount());
}
*/


uintptr_t CreationEngineRendererModule::setReflexMarkerInternal(uintptr_t rcx, uint32_t marker, uint32_t oldFrameIndex)
{
//    logWithExtraData(std::format("setReflexMarkerInternal[{}, f={}]", marker, oldFrameIndex));
    static auto instance = Get();
    static bool engine_notified = false;
    static     auto        vr            = VR::get();
    static auto cameraModule = CreationEngineCameraManager::Get();
    static bool            sync_marker_started = false;
    static int frames_since_reset = 0;
    if ((marker == 6 || marker == 0 || marker == 1) && !engine_notified) {
        engine_notified = true;
        instance->SetWindowSize(0,0);
        vr->on_wait_rendering(oldFrameIndex);
        vr->m_engine_frame_count = oldFrameIndex;
        vr->on_begin_rendering(oldFrameIndex);
        vr->update_hmd_state(oldFrameIndex);
        g_framework->run_imgui_frame(false);


        if(vr->get_runtime()->loaded) {
            frames_since_reset++;
            if(vr->m_engine_frame_count % 2 == 0) {
                sync_marker_started = true;
            } else {
                sync_marker_started = false;
            }
        }
        CreationEngineCameraManager::SnapshotAimPose();
        cameraModule->UpdateWorldCamera();
    }
    // Reset notification if marker is 1
    if (marker == 1) {
        engine_notified = false;
    }

    if (marker == 2) {
        vr->m_render_frame_count = oldFrameIndex;
    }

    if (marker == 4) {
        vr->m_presenter_frame_count = oldFrameIndex;
        PerfStats::OnPresent();
    }


    if(vr->get_runtime()->loaded && marker == 1 && sync_marker_started) {
        /*
         * as we sync on game loop L eye + frame + game loop R eye + frame Enc
         * we don't expect any async frames comes into this loop
         * if we detect async frame we reset sync and let engine to handle it
         */
        sync_marker_started = false;
    } else if(frames_since_reset > 100 && marker > 1 && marker < 5 && sync_marker_started && vr->get_runtime()->loaded) {
        spdlog::info("Detected frame inconsistency, resetting frame sync m={}", marker);
        PerfStats::OnFrameResync();
        vr->m_skip_next_present = true;
        frames_since_reset = 0;
        sync_marker_started = false;
    }
    return instance->m_setReflexMarkerInternalHook.call<uintptr_t>(rcx, marker, oldFrameIndex);
}
