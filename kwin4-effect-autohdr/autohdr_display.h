#pragma once

#include <core/backendoutput.h>
#include <core/output.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>

namespace AutoHdr {

inline bool isHdrOutput(const KWin::LogicalOutput *output)
{
    if (!output) {
        return false;
    }
    const KWin::BackendOutput *backend = output->backendOutput();
    return backend && backend->highDynamicRange();
}

inline bool windowOnHdrOutput(const KWin::EffectWindow *window)
{
    return window && isHdrOutput(window->screen());
}

inline KWin::LogicalOutput *outputForViewport(KWin::EffectsHandler *effects, const KWin::RenderViewport &viewport)
{
    if (!effects) {
        return nullptr;
    }
    return effects->screenAt(viewport.renderRect().center().toPoint());
}

inline bool shouldApplyHdrForPaint(KWin::EffectsHandler *effects, KWin::LogicalOutput *trackedOutput,
                                   const KWin::RenderViewport &viewport)
{
    KWin::LogicalOutput *output = trackedOutput;
    if (!output) {
        output = outputForViewport(effects, viewport);
    }
    return isHdrOutput(output);
}

} // namespace AutoHdr
