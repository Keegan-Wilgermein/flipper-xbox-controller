import Foundation

/// The state of an Xbox Wireless Controller (model 1914, Bluetooth LE) and its 16-byte input report 1,
/// laid out as received by a real controller:
/// LX, LY, RX, RY (u16, 0x8000 centre, y down), LT, RT (10 bits in u16), hat (1–8 clockwise from up, 0 none),
/// buttons (u16: A 0, B 1, X 3, Y 4, LB 6, RB 7, View 10, Menu 11, Xbox 12, L3 13, R3 14), Share (bit 0).
public struct XboxPad: Equatable, Sendable {
    public enum Button: Hashable, Sendable {
        case a, b, x, y, lb, rb, lt, rt, view, menu, l3, r3, xbox, share, dpadUp, dpadDown, dpadLeft, dpadRight
    }

    public var buttons: Set<Button> = []
    /// -1…1, y down.
    public var left = (x: 0.0, y: 0.0)
    public var right = (x: 0.0, y: 0.0)

    public init() {}

    public static func == (a: XboxPad, b: XboxPad) -> Bool {
        a.buttons == b.buttons && a.left == b.left && a.right == b.right
    }

    public var report: [UInt8] {
        var r = [UInt8](repeating: 0, count: 16)
        func axis(_ v: Double, _ at: Int) {
            let u = UInt16(((min(max(v, -1), 1) + 1) / 2 * 65535).rounded())
            r[at] = UInt8(u & 0xFF)
            r[at + 1] = UInt8(u >> 8)
        }
        axis(left.x, 0); axis(left.y, 2); axis(right.x, 4); axis(right.y, 6)
        if buttons.contains(.lt) { r[8] = 0xFF; r[9] = 0x03 }
        if buttons.contains(.rt) { r[10] = 0xFF; r[11] = 0x03 }
        r[12] = hat
        let bits: [Button: Int] = [.a: 0, .b: 1, .x: 3, .y: 4, .lb: 6, .rb: 7, .view: 10, .menu: 11,
                                      .xbox: 12, .l3: 13, .r3: 14]
        for (button, bit) in bits where buttons.contains(button) { r[13 + bit / 8] |= 1 << UInt8(bit % 8) }
        if buttons.contains(.share) { r[15] = 1 }
        return r
    }

    /// 1 = up, then clockwise to 8 = up-left; 0 = released. Opposite directions cancel.
    private var hat: UInt8 {
        let x = (buttons.contains(.dpadRight) ? 1 : 0) - (buttons.contains(.dpadLeft) ? 1 : 0)
        let y = (buttons.contains(.dpadDown) ? 1 : 0) - (buttons.contains(.dpadUp) ? 1 : 0)
        switch (x, y) {
        case (0, -1): return 1
        case (1, -1): return 2
        case (1, 0): return 3
        case (1, 1): return 4
        case (0, 1): return 5
        case (-1, 1): return 6
        case (-1, 0): return 7
        case (-1, -1): return 8
        default: return 0
        }
    }
}
