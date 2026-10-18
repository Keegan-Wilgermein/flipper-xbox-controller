// Logs every change in an Xbox controller's raw input reports (report bytes in hex, with time)
import Foundation
import IOKit.hid

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [kIOHIDVendorIDKey: 0x045E] as CFDictionary)
var last = [UInt8]()
let start = Date()
let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
IOHIDManagerRegisterDeviceMatchingCallback(manager, { _, _, _, device in
    print("device:", IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) ?? "?")
    IOHIDDeviceRegisterInputReportCallback(device, buffer, 64, { _, _, _, _, id, report, length in
        let bytes = Array(UnsafeBufferPointer(start: report, count: length))
        // Ignore stick jitter: compare everything except the four stick words.
        let key = Array(bytes.dropFirst(8))
        if key != last {
            last = key
            print(String(format: "%6.2f", Date().timeIntervalSince(start)), "id", id, bytes.map { String(format: "%02x", $0) }.joined(separator: " "))
            fflush(stdout)
        }
    }, nil)
}, nil)
IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
print("open:", IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone)))
fflush(stdout)
RunLoop.main.run(until: Date().addingTimeInterval(Double(CommandLine.arguments.dropFirst().first ?? "90") ?? 90))
