import Foundation
import UIKit

/// A Mac made on the Mac (HengeVM or Henge) and brought over: its boot ROM,
/// disk and auxiliary storage, and the identity it was installed with.
struct VirtualMac: Identifiable, Codable, Hashable {
    var id = UUID()
    var name: String
    var ecid: UInt64
    var seed: UInt64
    var ramMB: Int
    var diskBytes: Int64
    var added = Date()
    var osName: String?
    var lastUsed: Date?
    /// The size macOS is told its screen is. Macs made before this was a choice have none: 16:9.
    var screenWidth: Int?
    var screenHeight: Int?
    /// m4 fork: the iPad's own panel at Retina density. On by default on iPad, for Macs that never chose.
    var retina: Bool?
    var isRetina: Bool { Device.isPad && (retina ?? (screenWidth == nil)) }
    var screen: (width: Int, height: Int) { isRetina ? Device.retinaScreen : (screenWidth ?? 1920, screenHeight ?? 1080) }
    var aspect: CGFloat { CGFloat(screen.width) / CGFloat(screen.height) }

    var directory: URL { Machines.directory(for: id) }
    var rom: URL { directory.appendingPathComponent("AVPBooter.vmapple2.bin") }
    var disk: URL { directory.appendingPathComponent("disk.img") }
    var aux: URL { directory.appendingPathComponent("auxiliary.bin") }
}

/// How much RAM a Mac gets. The engine first asks the kernel to leave it out of Henge's footprint,
/// which is allowed once a debugger has attached (how JIT is turned on). If that is refused it uses
/// a file beside the disk: iOS's memory limit does not count a mapped file's pages either.
enum GuestMemory {
    /// Points the engine at the file (it is unlinked as soon as it is open) and returns the size that fits.
    static func prepare(_ mac: VirtualMac) -> UInt64 {
        let file = mac.directory.appendingPathComponent("ram.tmp")
        setenv("HV_RAM_FILE", file.path, 1)
        let free = (try? mac.directory.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
        let room = free > 0 ? UInt64(max(free - (2 << 30), 0)) >> 20 : UInt64.max
        let reach = henge_largest_mapping_mb()
        let ram = min(UInt64(mac.ramMB), room, reach > 512 ? reach - 512 : 0) / 512 * 512
        HengeLog.log("session", "guest RAM \(ram) MB (asked \(mac.ramMB), disk room \(room), largest mapping \(reach))")
        return ram
    }
}

/// machine.json in an exported folder (tools/export_ios.sh writes it).
private struct MachineFile: Codable {
    var name: String?
    var ecid: UInt64
    var seed: UInt64?
    var ramMB: Int?
}

@MainActor
final class Machines: ObservableObject {
    static let shared = Machines()

    @Published private(set) var all: [VirtualMac] = []
    @Published private(set) var importing: String?
    @Published var lastError: String?

    /// The files an exported machine folder holds; its disk is disk.img, or disk.img.hvsparse (tools/export_ios.sh packs it).
    static let required = ["AVPBooter.vmapple2.bin", "auxiliary.bin", "machine.json"]
    static let packedDisk = "disk.img.hvsparse"

    nonisolated static func hasDisk(_ folder: URL) -> Bool {
        let fm = FileManager.default
        return fm.fileExists(atPath: folder.appendingPathComponent("disk.img").path)
            || fm.fileExists(atPath: folder.appendingPathComponent(packedDisk).path)
    }

    @Published private(set) var unpacked: Double = 0
    /// While a Mac is being exported: its name and how far along.
    @Published private(set) var exporting: String?
    @Published private(set) var exported: Double = 0
    /// The last finished export, ready to be moved out of Henge.
    @Published var lastExport: URL?

    private init() {
        load()
        removeLeftovers()
    }

    /// Machine folders nothing refers to (an install cut short by a crash or by quitting): their disks take real space.
    private func removeLeftovers() {
        let known = Set(all.map(\.id))
        let items = (try? FileManager.default.contentsOfDirectory(at: Self.root, includingPropertiesForKeys: nil)) ?? []
        for item in items {
            guard let id = UUID(uuidString: item.lastPathComponent), !known.contains(id) else { continue }
            HengeLog.log("machines", "removing the unfinished machine \(id)")
            try? FileManager.default.removeItem(at: item)
        }
    }

    nonisolated static var root: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("Machines", isDirectory: true)
    }
    nonisolated static func directory(for id: UUID) -> URL { root.appendingPathComponent(id.uuidString, isDirectory: true) }
    /// Where Finder file sharing puts what the user drops on Henge.
    nonisolated static var inbox: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }

    /// Folders dropped into Henge's Documents over Finder (or the Files app) that look like
    /// an exported machine are moved in. A move, not a copy: a disk image is tens of gigabytes.
    func collectImports() {
        Task { await collect() }
    }

    private func collect() async {
        guard importing == nil, let items = try? FileManager.default.contentsOfDirectory(at: Self.inbox, includingPropertiesForKeys: nil) else { return }
        for folder in items where Self.hasDisk(folder) && Self.required.allSatisfy({ FileManager.default.fileExists(atPath: folder.appendingPathComponent($0).path) }) {
            await adopt(folder, move: true)
        }
    }

    /// A folder chosen in the Files picker: copied (it may be on another volume).
    func importFolder(_ folder: URL) {
        Task {
            let scoped = folder.startAccessingSecurityScopedResource()
            defer { if scoped { folder.stopAccessingSecurityScopedResource() } }
            await adopt(folder, move: false)
        }
    }

    private func adopt(_ folder: URL, move: Bool) async {
        do {
            let data = try Data(contentsOf: folder.appendingPathComponent("machine.json"))
            let info = try JSONDecoder().decode(MachineFile.self, from: data)
            let missing = Self.required.filter { !FileManager.default.fileExists(atPath: folder.appendingPathComponent($0).path) }
            guard missing.isEmpty, Self.hasDisk(folder) else { throw message("The folder is missing \((missing + (Self.hasDisk(folder) ? [] : ["disk.img"])).joined(separator: ", ")).") }
            var mac = VirtualMac(name: info.name ?? folder.lastPathComponent, ecid: info.ecid, seed: info.seed ?? 0,
                                 ramMB: info.ramMB ?? 2048, diskBytes: 0)
            importing = mac.name
            unpacked = 0
            let dest = mac.directory
            let fm = FileManager.default
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            excludeFromBackup()
            for name in ["AVPBooter.vmapple2.bin", "auxiliary.bin", "machine.json", "disk.img"] {
                let from = folder.appendingPathComponent(name)
                guard fm.fileExists(atPath: from.path) else { continue }
                if move { try fm.moveItem(at: from, to: dest.appendingPathComponent(name)) }
                else { try fm.copyItem(at: from, to: dest.appendingPathComponent(name)) }
            }
            let packed = folder.appendingPathComponent(Self.packedDisk)
            if fm.fileExists(atPath: packed.path) {
                // Off the main thread: unpacking tens of gigabytes takes minutes.
                let target = mac.disk.path, source = packed.path
                let ok = await Task.detached { () -> Bool in
                    hv_sparse_unpack(source, target, { done, total, _ in
                        let fraction = Double(done) / Double(max(total, 1))
                        Task { @MainActor in Machines.shared.unpacked = fraction }
                    }, nil) == 0
                }.value
                guard ok else { try? fm.removeItem(at: dest); throw message("The packed disk could not be unpacked (not enough space?).") }
                if move { try? fm.removeItem(at: packed) }
            }
            if move { try? fm.removeItem(at: folder) }
            mac.diskBytes = (try? fm.attributesOfItem(atPath: mac.disk.path)[.size] as? Int64) ?? 0
            all.append(mac)
            save()
            HengeLog.log("machines", "imported \(mac.name) (ECID \(mac.ecid), \(mac.diskBytes >> 30) GB disk)")
        } catch {
            lastError = error.localizedDescription
            HengeLog.log("machines", "import of \(folder.lastPathComponent) failed: \(error.localizedDescription)")
        }
        importing = nil
    }

    /// Makes a folder "Bring in a Mac" can read back (the same files tools/export_ios.sh writes),
    /// in Documents/Exports, which the Files app can reach. Deleting Henge deletes that folder too,
    /// so it has to be moved out of Henge before a reinstall that replaces the app's data.
    func export(_ mac: VirtualMac) {
        guard exporting == nil, importing == nil else { return }
        exporting = mac.name
        exported = 0
        lastExport = nil
        lastError = nil
        UIApplication.shared.isIdleTimerDisabled = true // a locked screen would suspend the app mid-way
        Task {
            defer {
                exporting = nil
                UIApplication.shared.isIdleTimerDisabled = false
            }
            do { lastExport = try await writeExport(mac) } catch {
                lastError = error.localizedDescription
                HengeLog.log("machines", "export of \(mac.name) failed: \(error.localizedDescription)")
            }
        }
    }

    private func writeExport(_ mac: VirtualMac) async throws -> URL {
        let fm = FileManager.default
        let exports = Self.inbox.appendingPathComponent("Exports", isDirectory: true)
        let safeName = mac.name.replacingOccurrences(of: "/", with: "-")
        let final = exports.appendingPathComponent(safeName, isDirectory: true)
        // Not under its final name until it is whole, so a cut-short export can't be mistaken for a Mac.
        let partial = exports.appendingPathComponent(safeName + ".partial", isDirectory: true)
        try? fm.removeItem(at: partial)
        try? fm.removeItem(at: final)
        let used = (try? mac.disk.resourceValues(forKeys: [.totalFileAllocatedSizeKey]))?.totalFileAllocatedSize ?? 0
        let free = (try? Self.root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage ?? 0
        let needed = Int64(used) + (256 << 20)
        if free > 0 && free < needed {
            throw message("Exporting \(mac.name) needs about \(ByteText.string(UInt64(needed))) free; this \(Device.name) has \(ByteText.string(UInt64(free))). Deleting a downloaded macOS installer (Install macOS, swipe it) frees about 16 GB.")
        }
        try fm.createDirectory(at: partial, withIntermediateDirectories: true)
        do {
            try fm.copyItem(at: mac.rom, to: partial.appendingPathComponent("AVPBooter.vmapple2.bin"))
            try fm.copyItem(at: mac.aux, to: partial.appendingPathComponent("auxiliary.bin"))
            let info: [String: Any] = ["name": mac.name, "ecid": mac.ecid, "seed": mac.seed, "ramMB": mac.ramMB]
            try JSONSerialization.data(withJSONObject: info).write(to: partial.appendingPathComponent("machine.json"))
            let source = mac.disk.path, target = partial.appendingPathComponent(Self.packedDisk).path
            let ok = await Task.detached { () -> Bool in
                hv_sparse_pack_progress(source, target, { done, total, _ in
                    let fraction = Double(done) / Double(max(total, 1))
                    Task { @MainActor in Machines.shared.exported = fraction }
                }, nil) == 0
            }.value
            guard ok else { throw message("The disk could not be written (not enough space?).") }
            try fm.moveItem(at: partial, to: final)
        } catch {
            try? fm.removeItem(at: partial)
            throw error
        }
        HengeLog.log("machines", "exported \(mac.name) to \(final.path)")
        return final
    }

    /// A machine made here (the installer), already in its directory.
    func add(_ mac: VirtualMac) {
        all.removeAll { $0.id == mac.id }
        all.append(mac)
        save()
    }

    func update(_ mac: VirtualMac) {
        guard let i = all.firstIndex(where: { $0.id == mac.id }) else { return }
        all[i] = mac
        save()
    }

    func delete(_ id: UUID) {
        try? FileManager.default.removeItem(at: Self.directory(for: id))
        all.removeAll { $0.id == id }
        save()
    }

    private var listURL: URL { Self.root.appendingPathComponent("machines.json") }

    /// Disk images are tens of gigabytes: keep them out of iCloud and computer backups.
    private func excludeFromBackup() { Self.excludeFromBackup(Self.root) }

    nonisolated static func excludeFromBackup(_ dir: URL) {
        var url = dir
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? url.setResourceValues(values)
    }

    private func load() {
        guard let data = try? Data(contentsOf: listURL), let list = try? JSONDecoder().decode([VirtualMac].self, from: data) else { return }
        all = list.filter { FileManager.default.fileExists(atPath: $0.disk.path) }
    }

    private func save() {
        try? FileManager.default.createDirectory(at: Self.root, withIntermediateDirectories: true)
        excludeFromBackup()
        if let data = try? JSONEncoder().encode(all) { try? data.write(to: listURL, options: .atomic) }
    }

    private func message(_ text: String) -> NSError {
        NSError(domain: "Henge", code: 1, userInfo: [NSLocalizedDescriptionKey: text])
    }
}
