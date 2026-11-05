import SwiftUI
import UniformTypeIdentifiers

struct HomeView: View {
    @ObservedObject private var machines = Machines.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var installer = Installer.shared
    @Binding var running: VirtualMac?
    @State private var picking = false
    @State private var installing = false
    @State private var jitReady = JITBootstrap.ready
    @State private var toDelete: VirtualMac?
    @State private var movingExport = false

    var body: some View {
        NavigationStack {
            List {
                Section("JIT") {
                    LabeledContent("Status", value: jitReady ? "On" : "Off")
                    if !jitReady {
                        Text(JITBootstrap.lastFailure ?? jit.automaticDescription)
                            .font(.footnote).foregroundStyle(.secondary)
                        Button("Turn On") { jit.enable() }
                    }
                }
                Section("Memory") {
                    LabeledContent("Extra memory", value: Entitlements.increasedMemory ? "On" : "Off")
                    LabeledContent("Henge's limit", value: ByteText.string(UInt64(Entitlements.memoryLimitAtLaunch)))
                    Text("A Mac's memory lives in a file, so it can go past this limit. It uses that much free storage while the Mac runs.")
                        .font(.footnote).foregroundStyle(.secondary)
                    if !Entitlements.increasedMemory {
                        Text("This install isn't signed with the increased memory limit entitlement. Reinstall Henge with a certificate that has it. Export your Mac first if the reinstall replaces the app's data.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                if let name = machines.exporting {
                    Section {
                        ProgressView("Exporting \(name)… keep Henge open", value: machines.exported)
                    }
                }
                if let folder = machines.lastExport {
                    Section("Exported") {
                        Text("Saved in Files, under Henge, Exports, \(folder.lastPathComponent). Deleting Henge deletes it too, so move it somewhere else first.")
                            .font(.footnote)
                        Button("Move Out of Henge…") { movingExport = true }
                    }
                }
                if let name = machines.importing {
                    Section {
                        HStack {
                            ProgressView()
                            Text(machines.unpacked > 0 ? String(format: "Unpacking %@'s disk… %.0f%%", name, machines.unpacked * 100)
                                                       : "Bringing in \(name)…")
                        }
                    }
                }
                if let error = machines.lastError {
                    Section { Text(error).foregroundStyle(.red) }
                }
                if installer.busy {
                    Section {
                        Button("Installing \(installer.machineName)… (tap to see)") { installing = true }
                    }
                }
                Section("Macs") {
                    if machines.all.isEmpty {
                        Button("Install macOS") { installing = true }
                    }
                    ForEach(machines.all) { mac in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(mac.name)
                                Text("\(mac.ramMB / 1024) GB memory, \(ByteText.string(UInt64(max(mac.diskBytes, 0)))) disk")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Start") { running = mac }
                                .buttonStyle(.bordered)
                                .disabled(!jitReady)
                            Menu {
                                Menu("Memory: \(mac.ramMB / 1024) GB") {
                                    ForEach([4, 6, 8], id: \.self) { gb in
                                        Button("\(gb) GB") {
                                            var changed = mac
                                            changed.ramMB = gb * 1024
                                            machines.update(changed)
                                        }
                                    }
                                }
                                Button("Export") { machines.export(mac) }
                                    .disabled(machines.exporting != nil)
                                Button("Delete", role: .destructive) { toDelete = mac }
                            } label: {
                                Image(systemName: "ellipsis.circle")
                            }
                            .accessibilityLabel("More for \(mac.name)")
                        }
                    }
                }
            }
            .navigationTitle("Henge")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Menu {
                        Button("Install macOS…") { installing = true }
                        Button("Bring in a Mac from a computer…") { picking = true }
                    } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel("Add a Mac")
                }
            }
            .sheet(isPresented: $installing) { InstallView() }
            .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
                if case let .success(url) = result { machines.importFolder(url) }
            }
            .fileMover(isPresented: $movingExport, file: machines.lastExport) { result in
                switch result {
                case .success: machines.lastExport = nil
                case .failure(let error): machines.lastError = error.localizedDescription
                }
            }
            .confirmationDialog("Delete \(toDelete?.name ?? "this Mac")?", isPresented: Binding(get: { toDelete != nil }, set: { if !$0 { toDelete = nil } }),
                                titleVisibility: .visible) {
                Button("Delete", role: .destructive) {
                    if let toDelete { machines.delete(toDelete.id) }
                }
            } message: {
                Text("Its disk is removed from this iPhone.")
            }
            .task {
                while !Task.isCancelled { // the region may be claimed after a hand-off to StikDebug
                    jitReady = JITBootstrap.ready
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }
}
