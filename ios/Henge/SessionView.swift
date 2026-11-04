import SwiftUI

/// A running Mac. There is no screen yet: iOS has no ParavirtualizedGraphics,
/// so the guest's display is still to come; this shows the machine's progress
/// and the log instead.
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
        ZStack {
            Stone.bezel.ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text(mac.name).font(.stone(24)).foregroundStyle(Stone.bezelText)
                    Spacer()
                    Button(state == 1 ? "Shut down" : "Close") { finish() }
                        .buttonStyle(.bordered)
                        .tint(Stone.copper)
                }
                screen
                if let error {
                    Text(error).foregroundStyle(Stone.copper).font(.subheadline)
                }
                status
                logView
            }
            .padding(20)
        }
        .task { await run() }
    }

    private var screen: some View {
        RoundedRectangle(cornerRadius: 18, style: .continuous)
            .fill(Color.black)
            .aspectRatio(16.0 / 10.0, contentMode: .fit)
            .overlay(BootDisplay().clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous)))
            .overlay(alignment: .bottom) {
                VStack(spacing: 2) {
                    Text(phase).font(.headline).foregroundStyle(Stone.bezelText)
                    Text("Boot screen only: the full display needs a GPU iOS does not have yet.").font(.caption2).foregroundStyle(Stone.bezelDim)
                }
                .padding(8)
                .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 10))
                .padding(8)
            }
    }

    private var phase: String {
        switch state {
        case 0: return error == nil ? "Starting…" : "Not started"
        case 1: return userCode ? "macOS is running" : "Booting"
        default: return "Stopped (\(String(cString: henge_vm_exit_reason())))"
        }
    }

    private var status: some View {
        VStack(alignment: .leading, spacing: 6) {
            row("CPUs on", "\(cpusOn) of \(cpus)")
            row("Translated blocks", String(format: "%.1f M/s", blocksPerSecond / 1e6))
            row("Network frames", "\(framesOut) out, \(framesIn) in")
            row("Up", "\(Int(Date().timeIntervalSince(started))) s")
        }
        .font(.system(.footnote, design: .monospaced))
        .foregroundStyle(Stone.bezelText)
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack { Text(label).foregroundStyle(Stone.bezelDim); Spacer(); Text(value) }
    }

    private var logView: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(log.enumerated()), id: \.offset) { i, line in
                        Text(line).font(.system(size: 10, design: .monospaced)).foregroundStyle(Stone.bezelDim).id(i)
                    }
                }
            }
            .onChange(of: log.count) { n in proxy.scrollTo(n - 1, anchor: .bottom) }
        }
    }

    /// Guest RAM: what the machine was made with, or less if iOS will not give the app that much.
    /// The translator's own tables (blocks, hash, slots) take about 400 MB more.
    private func guestRAM() -> UInt64 {
        let available = UInt64(henge_available_memory()) >> 20
        let fits = available > 1400 ? (available - 900) / 512 * 512 : 0
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
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    private func finish() {
        if state == 1 { henge_vm_stop() }
        close()
    }
}
