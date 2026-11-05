import SwiftUI

/// A running Mac: its screen, how it is doing, and the log. No keyboard or mouse yet.
struct SessionView: View {
    let mac: VirtualMac
    let close: () -> Void

    @State private var error: String?
    @State private var state: Int32 = 0
    @State private var cpusOn: Int32 = 0
    @State private var userCode = false
    @State private var blocksPerSecond: Double = 0
    @State private var framesOut: UInt64 = 0
    @State private var framesIn: UInt64 = 0
    @State private var log: [String] = []
    @State private var started = Date()
    @AppStorage("henge.cpus") private var cpus = 3

    var body: some View {
        NavigationStack {
            List {
                Section(phase) {
                    BootDisplay().aspectRatio(16.0 / 10.0, contentMode: .fit)
                    if let error { Text(error).foregroundStyle(.red) }
                }
                Section("Status") { status }
                Section("Log") { logView }
            }
            .navigationTitle(mac.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button(state == 1 ? "Shut Down" : "Close") { finish() }
                }
            }
        }
        .task { await run() }
    }

    private var phase: String {
        switch state {
        case 0: return error == nil ? "Starting…" : "Not started"
        case 1: return userCode ? "macOS is running" : "Booting"
        default: return "Stopped (\(String(cString: henge_vm_exit_reason())))"
        }
    }

    @ViewBuilder private var status: some View {
        LabeledContent("CPUs on", value: "\(cpusOn) of \(cpus)")
        LabeledContent("Translated blocks", value: String(format: "%.1f M/s", blocksPerSecond / 1e6))
        LabeledContent("Network frames", value: "\(framesOut) out, \(framesIn) in")
        LabeledContent("Up", value: "\(Int(Date().timeIntervalSince(started))) s")
    }

    private var logView: some View {
        VStack(alignment: .leading, spacing: 1) {
            ForEach(Array(log.suffix(30).enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
            }
        }
    }

    /// Guest RAM: what the machine was made with, or less if iOS will not give the app that much.
    /// The translator's own tables (blocks, hash, slots) take about 400 MB more.
    private func guestRAM() -> UInt64 {
        let available = UInt64(henge_available_memory()) >> 20
        let fits = available > 2000 ? (available - 1500) / 512 * 512 : 0 // the phone's log: 1.5 GB is not too much once the GPU is drawing
        return min(UInt64(mac.ramMB), fits)
    }

    private func run() async {
        let ram = guestRAM()
        HengeLog.log("session", "available memory \(henge_available_memory() >> 20) MB; guest gets \(ram) MB")
        guard ram >= 1024 else {
            error = "Only \(henge_available_memory() >> 20) MB is free for Henge. A Mac needs at least 1 GB, more with the increased memory limit entitlement."
            return
        }
        var buffer = [CChar](repeating: 0, count: 256)
        let rc = henge_vm_boot(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, mac.seed, ram, Int32(cpus), &buffer, buffer.count)
        if rc != 0 {
            error = String(cString: buffer)
            return
        }
        started = Date()
        var lastBlocks = henge_vm_blocks()
        var lastTime = Date()
        var tick = 0
        while !Task.isCancelled {
            state = henge_vm_state()
            cpusOn = henge_vm_cpus_on()
            userCode = userCode || henge_vm_guest_el0() != 0
            let now = Date(), blocks = henge_vm_blocks()
            blocksPerSecond = Double(blocks &- lastBlocks) / max(now.timeIntervalSince(lastTime), 0.001)
            lastBlocks = blocks
            lastTime = now
            framesOut = henge_vm_net_frames(1)
            framesIn = henge_vm_net_frames(0)
            log = Array(HengeLog.recentLines(200))
            if state == 2 { break }
            tick += 1
            if tick % 30 == 0 { HengeLog.logFootprint("session") } // how close to the limit, for a memory kill leaves no crash report
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    private func finish() {
        if state == 1 { henge_vm_stop() }
        close()
    }
}
