import SwiftUI

struct SettingsView: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @AppStorage(JITCoordinator.autoEnableKey) private var jitAtLaunch = false
    @AppStorage(HardwareMouse.speedKey) private var mouseSpeed = 1.0
    @AppStorage(HardwareMouse.reverseKey) private var reverseScrolling = false

    var body: some View {
        NavigationStack {
            Form {
                Section("JIT") {
                    LabeledContent("Status", value: JITBootstrap.ready ? "On" : "Off")
                    Picker("Method", selection: $jit.method) {
                        ForEach(JITMethod.allCases) { Text($0.title).tag($0) }
                    }
                    Text(jit.method == .automatic ? jit.automaticDescription : "Henge uses \(jit.method.title).")
                        .font(.footnote).foregroundStyle(.secondary)
                    Button("Set up JIT") { jit.showSetup = true }
                    Toggle("Turn on JIT when Henge opens", isOn: $jitAtLaunch)
                    LabeledContent("Code region", value: "\(JITBootstrap.jitBytes >> 20) MB, claimed once per launch")
                }
                .listRowBackground(Desk.card)
                Section("Memory") {
                    LabeledContent("Extra memory", value: Entitlements.increasedMemory ? "On" : "Off")
                    LabeledContent("Henge's limit", value: ByteText.string(UInt64(Entitlements.memoryLimitAtLaunch)))
                    LabeledContent("Free for Henge", value: ByteText.string(UInt64(henge_available_memory())))
                    Text(Entitlements.increasedMemory
                         ? "A Mac's memory is kept out of this limit once JIT is on. If iOS refuses that, it uses free storage instead."
                         : "This install isn't signed with the increased memory limit entitlement. Reinstall Henge with a certificate that has it. Export your Mac first if the reinstall replaces the app's data.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                .listRowBackground(Desk.card)
                if Device.isPad {
                    Section("Mouse and Trackpad") {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Pointer speed")
                            Slider(value: $mouseSpeed, in: 0.5...3)
                        }
                        Toggle("Reverse scrolling", isOn: $reverseScrolling)
                        Text("A Magic Keyboard or a Bluetooth mouse moves the Mac's pointer. Changes apply the next time a Mac starts.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                    .listRowBackground(Desk.card)
                }
                Section("Log") {
                    ShareLink(item: HengeLog.logFileURL) { Label("Share henge.log", systemImage: "square.and.arrow.up") }
                }
                .listRowBackground(Desk.card)
            }
            .scrollContentBackground(.hidden)
            .background(Desk.page)
            .navigationTitle("Settings")
        }
    }
}
