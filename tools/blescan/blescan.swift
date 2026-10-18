// Prints the full advertisement of every Bluetooth LE device whose name contains "Xbox".
import CoreBluetooth
import Foundation

final class Scanner: NSObject, CBCentralManagerDelegate {
    var central: CBCentralManager!
    var seen: [UUID: String] = [:]
    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        print("bluetooth state:", c.state.rawValue); fflush(stdout)
        if c.state == .poweredOn { c.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
        let name = (ad[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        guard name.localizedCaseInsensitiveContains("xbox") else { return }
        var parts: [String] = ["name=\(name)", "rssi=\(rssi)"]
        for (k, v) in ad.sorted(by: { $0.key < $1.key }) {
            if let d = v as? Data { parts.append("\(k)=\(d.map { String(format: "%02x", $0) }.joined())") }
            else { parts.append("\(k)=\(v)") }
        }
        let line = parts.joined(separator: " | ")
        if seen[p.identifier] != line { seen[p.identifier] = line; print(p.identifier, line); fflush(stdout) }
    }
}
// Output goes to the file given as the second argument (an app launched with `open` has no terminal).
if CommandLine.arguments.count > 2 { freopen(CommandLine.arguments[2], "w", stdout) }
let s = Scanner()
RunLoop.main.run(until: Date().addingTimeInterval(Double(CommandLine.arguments.dropFirst().first ?? "60") ?? 60))
_ = s
