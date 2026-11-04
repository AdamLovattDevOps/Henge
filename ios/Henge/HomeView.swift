import SwiftUI
import UniformTypeIdentifiers

struct HomeView: View {
    @ObservedObject private var machines = Machines.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @Binding var running: VirtualMac?
    @State private var picking = false
    @State private var jitReady = JITBootstrap.ready

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    header
                    jitStrip
                    if let name = machines.importing {
                        progress(machines.unpacked > 0 ? String(format: "Unpacking %@'s disk… %.0f%%", name, machines.unpacked * 100)
                                                       : "Bringing in \(name)…")
                    }
                    if let error = machines.lastError { note(error) }
                    if machines.all.isEmpty {
                        welcome
                    } else {
                        ForEach(machines.all) { mac in
                            MachineCard(mac: mac, jitOn: jitReady) { running = mac }
                        }
                    }
                }
                .padding(.horizontal, 20)
                .padding(.top, 12)
                .padding(.bottom, 28)
            }
            .background(Stone.page.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
            .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
                if case let .success(url) = result { machines.importFolder(url) }
            }
            .task {
                while !Task.isCancelled { // the region may be claimed after a hand-off to StikDebug
                    jitReady = JITBootstrap.ready
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }

    private var header: some View {
        HStack(alignment: .center, spacing: 12) {
            StonesMark(height: 32)
            Text("Henge")
                .font(.stone(34))
                .foregroundStyle(Stone.text)
            Spacer()
            Button { picking = true } label: {
                Image(systemName: "plus")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(Stone.text)
                    .frame(width: 40, height: 40)
                    .background(Stone.raised, in: Circle())
            }
            .accessibilityLabel("Bring in a Mac")
        }
    }

    private var jitStrip: some View {
        HStack(alignment: .center, spacing: 12) {
            Circle()
                .fill(jitReady ? Stone.lichen : Color.clear)
                .overlay(Circle().stroke(jitReady ? Stone.lichen : Stone.copper, lineWidth: 2))
                .frame(width: 16, height: 16)
            VStack(alignment: .leading, spacing: 2) {
                Text(jitReady ? "JIT is on" : "JIT is off")
                    .font(.headline)
                    .foregroundStyle(Stone.text)
                Text(jitReady ? "A \(JITBootstrap.jitBytes >> 20) MB region is held for this session."
                              : (JITBootstrap.lastFailure ?? jit.automaticDescription))
                    .font(.subheadline)
                    .foregroundStyle(Stone.dim)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            if !jitReady {
                Button("Turn on") { jit.enable() }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .stoneCard()
    }

    private var welcome: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("A Mac, in the circle")
                .font(.stone(26))
                .foregroundStyle(Stone.text)
            Text("Make the Mac on your computer first (HengeVM or Henge), export it with tools/export_ios.sh, and drop the folder on Henge in Finder, or tap + and choose it in Files.")
                .font(.body)
                .foregroundStyle(Stone.dim)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .stoneCard()
    }

    private func progress(_ text: String) -> some View {
        HStack(spacing: 12) {
            ProgressView()
            Text(text).foregroundStyle(Stone.text)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .stoneCard()
    }

    private func note(_ text: String) -> some View {
        Text(text)
            .font(.subheadline)
            .foregroundStyle(Stone.copper)
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .stoneCard()
    }
}

private struct MachineCard: View {
    let mac: VirtualMac
    let jitOn: Bool
    let run: () -> Void
    @State private var confirmDelete = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Image(systemName: "desktopcomputer")
                    .font(.system(size: 22))
                    .foregroundStyle(Stone.copper)
                Text(mac.name)
                    .font(.stone(22))
                    .foregroundStyle(Stone.text)
                Spacer()
                Menu {
                    Button("Delete", role: .destructive) { confirmDelete = true }
                } label: {
                    Image(systemName: "ellipsis").foregroundStyle(Stone.dim).frame(width: 32, height: 32)
                }
            }
            Text("\(mac.ramMB / 1024) GB memory · \(ByteText.string(UInt64(max(mac.diskBytes, 0)))) disk")
                .font(.subheadline)
                .foregroundStyle(Stone.dim)
            Button(action: run) {
                Text(jitOn ? "Start" : "Start (turn on JIT first)")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!jitOn)
        }
        .padding(20)
        .stoneCard()
        .confirmationDialog("Delete \(mac.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Machines.shared.delete(mac.id) }
        } message: {
            Text("Its disk is removed from this iPhone.")
        }
    }
}
