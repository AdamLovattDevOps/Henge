import SwiftUI

/// Download macOS (or use a copy already on this iPhone) and install it on a new Mac.
struct InstallView: View {
    @ObservedObject private var installer = Installer.shared
    @Environment(\.dismiss) private var dismiss
    @State private var release = MacOSRelease.all.first(where: Installer.haveImage) ?? MacOSRelease.all[0]
    @State private var name = "Mac"
    @State private var ramGB = 4
    @State private var deleteAfter = false
    @State private var refresh = 0 // redraws the list when a download is deleted
    @AppStorage("henge.cpus") private var cpus = 3

    var body: some View {
        NavigationStack {
            Form {
                switch installer.phase {
                case .idle: chooser
                case .downloading: downloading
                case .preparing: Section { HStack { ProgressView(); Text("Making the new Mac…") } }
                case .installing: installing
                case .done(let text): Section("Done") { Text(text) }
                case .failed(let text): failed(text)
                }
            }
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

    @ViewBuilder private var chooser: some View {
        Section("Version") {
            ForEach(MacOSRelease.all) { r in
                Button { release = r } label: { versionRow(r) }
                    .buttonStyle(.plain)
                    .swipeActions {
                        if Installer.haveImage(r) {
                            Button("Delete Download", role: .destructive) {
                                Installer.deleteImage(r)
                                refresh += 1
                            }
                        }
                    }
            }
        }
        .id(refresh)
        Section("New Mac") {
            TextField("Name", text: $name)
            Stepper("Memory: \(ramGB) GB", value: $ramGB, in: 2...8)
            Toggle("Delete the download after installing", isOn: $deleteAfter)
        }
        Section {
            Button(Installer.haveImage(release) ? "Install" : "Download and Install") {
                installer.start(release, name: name.isEmpty ? "Mac" : name, ramMB: ramGB * 1024, cpus: cpus, keepImage: !deleteAfter)
            }
            .disabled(!JITBootstrap.ready)
        } footer: {
            Text(JITBootstrap.ready ? "Keep Henge open and the iPhone plugged in. Needs about \(ByteText.string(UInt64(Installer.spaceNeeded(release)))) free."
                                    : "Turn on JIT first.")
        }
    }

    private func versionRow(_ r: MacOSRelease) -> some View {
        HStack {
            VStack(alignment: .leading) {
                Text(r.name)
                if let size = Installer.imageSize(r) {
                    Text("Downloaded, \(ByteText.string(UInt64(size)))").font(.caption).foregroundStyle(.green)
                } else {
                    Text("\(ByteText.string(UInt64(r.bytes))) download").font(.caption).foregroundStyle(.secondary)
                }
                Text(r.note).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if release == r { Image(systemName: "checkmark").foregroundStyle(.tint) }
        }
        .contentShape(Rectangle())
    }

    private var downloading: some View {
        Section("Downloading \(release.name)") {
            ProgressView(value: installer.expected > 0 ? Double(installer.received) / Double(installer.expected) : 0)
            Text("\(ByteText.string(UInt64(installer.received))) of \(ByteText.string(UInt64(installer.expected)))")
                .font(.footnote.monospacedDigit())
        }
    }

    @ViewBuilder private var installing: some View {
        Section("Installing on \(installer.machineName)") {
            BootDisplay().aspectRatio(16.0 / 10.0, contentMode: .fit)
            Text(Installer.stepName(installer.step))
            ProgressView(value: installer.stepProgress)
        }
        Section("Log") { LogTail() }
    }

    @ViewBuilder private func failed(_ text: String) -> some View {
        Section("It did not work") {
            Text(text)
            Text("Quit Henge (swipe it away) and open it again before trying again: only one Mac can start per launch.")
                .font(.footnote).foregroundStyle(.secondary)
            Button("Back") { installer.reset() }
        }
        Section("Log") { LogTail() }
    }
}

/// The last lines of Henge's log.
private struct LogTail: View {
    @State private var lines: [String] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary).lineLimit(2)
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
