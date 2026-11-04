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
                        }
                        .swipeActions {
                            Button("Delete", role: .destructive) { toDelete = mac }
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
