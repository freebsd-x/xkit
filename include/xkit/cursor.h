#pragma once

#include <cstdint>

namespace fbx::xkit {

// Cursor shapes a widget can request while the pointer hovers it. The numeric
// values mirror the wp_cursor_shape_device_v1 enum exactly, so a value can be
// forwarded straight to the compositor (com.freebsdx.Input opcode 0x03) and on
// to DrmDevice::setCursorShape without translation.
//
// `Inherit` (0) is an xkit-only sentinel meaning "this node expresses no
// preference"; cursor resolution walks up to the nearest ancestor that set a
// real shape, falling back to `Default` (the arrow) if none did.
enum class CursorShape : uint32_t {
    Inherit      = 0,   // not set — inherit from ancestor (xkit-only)
    Default      = 1,   // arrow
    ContextMenu  = 2,
    Help         = 3,
    Pointer      = 4,   // hand — links, buttons
    Progress     = 5,
    Wait         = 6,
    Cell         = 7,
    Crosshair    = 8,
    Text         = 9,   // I-beam — editable text
    VerticalText = 10,
    Alias        = 11,
    Copy         = 12,
    Move         = 13,
    NoDrop       = 14,
    NotAllowed   = 15,
    Grab         = 16,
    Grabbing     = 17,
    EResize      = 18,
    NResize      = 19,
    NEResize     = 20,
    NWResize     = 21,
    SResize      = 22,
    SEResize     = 23,
    SWResize     = 24,
    WResize      = 25,
    EWResize     = 26,
    NSResize     = 27,
    NESWResize   = 28,
    NWSEResize   = 29,
    ColResize    = 30,
    RowResize    = 31,
    AllScroll    = 32,
    ZoomIn       = 33,
    ZoomOut      = 34,
};

} // namespace fbx::xkit
