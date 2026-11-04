import SwiftUI

struct SettingsView: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @AppStorage("henge.cpus") private var cpus = 3
    @AppStorage(JITCoordinator.autoEnableKey) private var jitAtLaunch = false

    var body: some View {
        NavigationStack {
            Form {
                Section("JIT") {
                    Picker("Method", selection: $jit.method) {
                        ForEach(JITMethod.allCases) { Text($0.title).tag($0) }
                    }
                    Text(jit.method == .automatic ? jit.automaticDescription : "Henge uses \(jit.method.title).")
                        .font(.footnote).foregroundStyle(Stone.dim)
                    Button("Set up JIT") { jit.showSetup = true }
                    Toggle("Turn on JIT when Henge opens", isOn: $jitAtLaunch)
                    LabeledContent("Region", value: "\(JITBootstrap.jitBytes >> 20) MB, claimed once per launch")
                }
                Section("Machine") {
                    Stepper("CPUs: \(cpus)", value: $cpus, in: 1...6)
                    Text("Three suits a 2 + 4 core iPhone: as fast as four, and the app keeps a core for itself.")
                        .font(.footnote).foregroundStyle(Stone.dim)
                    LabeledContent("Memory free for Henge", value: ByteText.string(UInt64(henge_available_memory())))
                }
                Section("Log") {
                    ShareLink(item: HengeLog.logFileURL) { Label("Share henge.log", systemImage: "square.and.arrow.up") }
                }
            }
            .navigationTitle("Settings")
        }
    }
}
