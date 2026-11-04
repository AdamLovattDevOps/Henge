import SwiftUI

/// Download macOS and install it on a new Mac, here on the iPhone.
struct InstallView: View {
    @ObservedObject private var installer = Installer.shared
    @Environment(\.dismiss) private var dismiss
    @State private var release = MacOSRelease.all[0]
    @State private var name = "Mac"
    @State private var ramGB = 4
    @State private var keepImage = false
    @AppStorage("henge.cpus") private var cpus = 3

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    switch installer.phase {
                    case .idle: chooser
                    case .downloading: downloading
                    case .preparing: working("Making the new Mac…", detail: "A 64 GB disk (it only takes the space macOS writes) and its startup storage.")
                    case .installing: installing
                    case .done(let text): finished(text, ok: true)
                    case .failed(let text): finished(text, ok: false)
                    }
                }
                .padding(20)
            }
            .background(Stone.page.ignoresSafeArea())
            .navigationTitle("Install macOS")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(installer.busy ? "Hide" : "Close") { dismiss() }
                }
            }
        }
        .interactiveDismissDisabled(installer.busy)
    }

    private var chooser: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Henge downloads the macOS installer from Apple and installs it on a new virtual Mac, the same way a Mac's Virtualization framework does: Apple's signing server approves the install for this machine, and macOS's own installer writes the disk.")
                .font(.subheadline).foregroundStyle(Stone.dim)
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 8) {
                Text("Version").font(.headline).foregroundStyle(Stone.text)
                ForEach(MacOSRelease.all) { r in
                    Button { release = r } label: {
                        HStack(alignment: .top) {
                            Image(systemName: release == r ? "largecircle.fill.circle" : "circle")
                                .foregroundStyle(Stone.copper)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(r.name).foregroundStyle(Stone.text)
                                Text("\(ByteText.string(UInt64(r.bytes))) download\(Installer.haveImage(r) ? " (already downloaded)" : "") · \(r.note)")
                                    .font(.caption).foregroundStyle(Stone.dim)
                            }
                            Spacer()
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(16).stoneCard()
            VStack(alignment: .leading, spacing: 10) {
                TextField("Name", text: $name)
                    .textFieldStyle(.roundedBorder)
                Stepper("Memory: \(ramGB) GB", value: $ramGB, in: 2...8)
                Toggle("Keep the installer for another Mac", isOn: $keepImage)
            }
            .padding(16).stoneCard()
            Text("Needs about \(ByteText.string(UInt64(release.bytes + (24 << 30)))) free. Keep Henge open and the iPhone plugged in: the download is large and the install takes a while. JIT must be on.")
                .font(.footnote).foregroundStyle(Stone.dim)
                .fixedSize(horizontal: false, vertical: true)
            Button {
                installer.start(release, name: name.isEmpty ? "Mac" : name, ramMB: ramGB * 1024, cpus: cpus, keepImage: keepImage)
            } label: {
                Text(JITBootstrap.ready ? "Download and Install" : "Download and Install (JIT is off)")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
        }
    }

    private var downloading: some View {
        let fraction = installer.expected > 0 ? Double(installer.received) / Double(installer.expected) : 0
        return VStack(alignment: .leading, spacing: 12) {
            Text("Downloading \(release.name)").font(.headline).foregroundStyle(Stone.text)
            ProgressView(value: fraction)
            Text("\(ByteText.string(UInt64(installer.received))) of \(ByteText.string(UInt64(installer.expected)))")
                .font(.footnote.monospacedDigit()).foregroundStyle(Stone.dim)
            Text("From Apple (updates.cdn-apple.com). If the connection drops, Henge picks up where it stopped.")
                .font(.footnote).foregroundStyle(Stone.dim)
        }
        .padding(16).stoneCard()
    }

    private var installing: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Installing on \(installer.machineName)").font(.headline).foregroundStyle(Stone.text)
            BootDisplay()
                .aspectRatio(16.0 / 10.0, contentMode: .fit)
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
            Text(Installer.stepName(installer.step)).foregroundStyle(Stone.text)
            ProgressView(value: installer.stepProgress)
            Text("The Mac starts its installer and macOS writes itself to the disk. This can take a long time on the iPhone.")
                .font(.footnote).foregroundStyle(Stone.dim)
                .fixedSize(horizontal: false, vertical: true)
            LogTail()
        }
        .padding(16).stoneCard()
    }

    private func working(_ title: String, detail: String) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) { ProgressView(); Text(title).foregroundStyle(Stone.text) }
            Text(detail).font(.footnote).foregroundStyle(Stone.dim)
        }
        .padding(16).stoneCard()
    }

    private func finished(_ text: String, ok: Bool) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(ok ? "Done" : "It did not work", systemImage: ok ? "checkmark.circle" : "exclamationmark.triangle")
                .font(.headline).foregroundStyle(ok ? Stone.lichen : Stone.copper)
            Text(text).foregroundStyle(Stone.text).fixedSize(horizontal: false, vertical: true)
            if !ok {
                LogTail()
                Button("Back") { installer.reset() }.buttonStyle(.bordered)
            }
        }
        .padding(16).stoneCard()
    }
}

/// The last lines of Henge's log, for the install screen.
private struct LogTail: View {
    @State private var lines: [String] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(size: 9, design: .monospaced)).foregroundStyle(Stone.dim).lineLimit(2)
            }
        }
        .task {
            while !Task.isCancelled {
                lines = Array(HengeLog.recentLines(14))
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }
}
