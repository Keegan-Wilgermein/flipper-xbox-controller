// Logs what macOS's GameController framework reports for every controller
import AppKit
import GameController

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
GCController.shouldMonitorBackgroundEvents = true
let start = Date()
func log(_ s: String) { print(String(format: "%6.2f ", Date().timeIntervalSince(start)) + s); fflush(stdout) }
func watch(_ c: GCController) {
    log("controller: \(c.vendorName ?? "?") category=\(c.productCategory) extended=\(c.extendedGamepad != nil)")
    c.physicalInputProfile.valueDidChangeHandler = { p, _ in
        let pressed = p.buttons.filter { $0.value.isPressed }.map(\.key).sorted()
        var axes: [String] = []
        for (k, a) in p.axes where abs(a.value) > 0.5 { axes.append(k) }
        log("pressed=\(pressed) axes=\(axes.sorted())")
    }
}
NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { n in
    if let c = n.object as? GCController { watch(c) }
}
GCController.controllers().forEach(watch)
log("watching")
DispatchQueue.main.asyncAfter(deadline: .now() + (Double(CommandLine.arguments.dropFirst().first ?? "120") ?? 120)) { exit(0) }
app.run()
