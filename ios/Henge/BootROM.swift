import Compression
import CryptoKit
import Foundation

/// The virtual Mac's boot ROM, AVPBooter. It is Apple's, so it isn't part of Henge: the app fetches it from Apple.
///
/// macOS keeps it in the system image, and since macOS 15 those are encrypted, but Apple's macOS 14.6.1 restore image
/// is not, and its ROM boots and installs later versions. The file there is one piece of one compressed block, so one
/// request gets all of it, under a megabyte out of 15 GB. The numbers below come from reading that image's disk layout;
/// the hash is what says they found the right bytes.
enum BootROM {
    private static let source = URL(string: "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw")!
    private static let blockOffset = 5_349_700_747, blockLength = 859_848, blockUnpacked = 1_048_576 // LZFSE
    private static let forkOffset = 876_544, forkLength = 116_335 // the file's data, compressed again block by block
    private static let romLength = 214_904
    private static let romSHA256 = "fd4973e42e609751830df6f3dd2d2b2ad2309fe38f0ca0f5aba02dcbefe45f91"

    enum Failure: LocalizedError {
        case download(String), damaged
        var errorDescription: String? {
            switch self {
            case .download(let why): return "Henge couldn't get the Mac's boot ROM from Apple (\(why)). Check the connection and try again."
            case .damaged: return "The Mac's boot ROM from Apple wasn't what Henge expected, so it wasn't used. Check the connection and try again, or look for an update to Henge."
            }
        }
    }

    /// The ROM on this device, fetched the first time it is needed.
    static func file() async throws -> URL {
        let folder = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("BootROM", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let file = folder.appendingPathComponent("AVPBooter.vmapple2.bin")
        if let data = try? Data(contentsOf: file), isGenuine(data) { return file }
        try await download().write(to: file, options: .atomic)
        return file
    }

    static func download() async throws -> Data {
        var request = URLRequest(url: source)
        request.setValue("bytes=\(blockOffset)-\(blockOffset + blockLength - 1)", forHTTPHeaderField: "Range")
        var lastFailure = "no answer"
        for attempt in 1...3 {
            do {
                let (data, response) = try await URLSession.shared.data(for: request)
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                guard status == 206, data.count == blockLength else {
                    lastFailure = "HTTP \(status), \(data.count) bytes"
                    continue
                }
                guard let rom = unpack(data), isGenuine(rom) else { throw Failure.damaged }
                return rom
            } catch let failure as Failure {
                throw failure
            } catch {
                lastFailure = error.localizedDescription
            }
            try await Task.sleep(nanoseconds: UInt64(attempt) * 1_000_000_000)
        }
        throw Failure.download(lastFailure)
    }

    private static func isGenuine(_ data: Data) -> Bool {
        guard data.count == romLength else { return false }
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() == romSHA256
    }

    private static func unpack(_ block: Data) -> Data? {
        var whole = [UInt8](repeating: 0, count: blockUnpacked)
        let got = block.withUnsafeBytes { (source: UnsafeRawBufferPointer) -> Int in
            guard let start = source.bindMemory(to: UInt8.self).baseAddress else { return 0 }
            return compression_decode_buffer(&whole, whole.count, start, block.count, nil, COMPRESSION_LZFSE)
        }
        guard got == blockUnpacked else { return nil }
        return fileFromFork(Array(whole[forkOffset..<forkOffset + forkLength]))
    }

    /// An APFS compressed file's resource fork: a table of where each 64 KB piece starts, then the pieces, in LZVN.
    private static func fileFromFork(_ fork: [UInt8]) -> Data? {
        func word(_ at: Int) -> Int {
            Int(fork[at]) | Int(fork[at + 1]) << 8 | Int(fork[at + 2]) << 16 | Int(fork[at + 3]) << 24
        }
        guard fork.count >= 4 else { return nil }
        let tableLength = word(0)
        guard tableLength >= 8, tableLength % 4 == 0, tableLength <= fork.count else { return nil }
        var out = Data(capacity: romLength)
        for piece in 0..<(tableLength / 4 - 1) {
            let start = word(piece * 4), end = word((piece + 1) * 4)
            guard start < end, end <= fork.count else { return nil }
            let want = min(65536, romLength - piece * 65536)
            guard want > 0 else { return nil }
            if fork[start] == 0x06 { // stored as it is
                out.append(contentsOf: fork[(start + 1)..<end])
            } else {
                guard let bytes = lzvn(Array(fork[start..<end]), expected: want) else { return nil }
                out.append(contentsOf: bytes)
            }
        }
        return out
    }

    /// LZVN, which Apple's compression library doesn't offer publicly. Each opcode gives some literal bytes to copy
    /// and a match to repeat from what is already out, at a distance that may be the same as the last one.
    static func lzvn(_ s: [UInt8], expected: Int) -> [UInt8]? {
        var out = [UInt8]()
        out.reserveCapacity(expected)
        var i = 0, distance = 0
        while i < s.count {
            let op = Int(s[i])
            var literal = 0, match = 0, length = 1
            func byte(_ at: Int) -> Int? { at < s.count ? Int(s[at]) : nil }
            switch op {
            case 0x06:
                return out.count == expected ? out : nil
            case 0x0e, 0x16:
                i += 1
                continue
            case 0x1e, 0x26, 0x2e, 0x36, 0x3e, 0x70...0x7f, 0xd0...0xdf:
                return nil
            case 0xe0:
                guard let n = byte(i + 1) else { return nil }
                literal = n + 16
                length = 2
            case 0xe1...0xef:
                literal = op & 15
            case 0xf0:
                guard let n = byte(i + 1) else { return nil }
                match = n + 16
                length = 2
            case 0xf1...0xff:
                match = op & 15
            case 0xa0...0xbf:
                guard let a = byte(i + 1), let b = byte(i + 2) else { return nil }
                let word = a | b << 8
                literal = (op >> 3) & 3
                match = (((op & 7) << 2) | (word & 3)) + 3
                distance = word >> 2
                length = 3
            default:
                literal = (op >> 6) & 3
                match = ((op >> 3) & 7) + 3
                switch op & 7 {
                case 6:
                    break // the same distance as last time
                case 7:
                    guard let a = byte(i + 1), let b = byte(i + 2) else { return nil }
                    distance = a | b << 8
                    length = 3
                default:
                    guard let a = byte(i + 1) else { return nil }
                    distance = (op & 7) << 8 | a
                    length = 2
                }
            }
            i += length
            guard i + literal <= s.count, out.count + literal + match <= expected else { return nil }
            out.append(contentsOf: s[i..<(i + literal)])
            i += literal
            if match > 0 {
                guard distance > 0, distance <= out.count else { return nil }
                for _ in 0..<match { out.append(out[out.count - distance]) } // may overlap what it writes
            }
        }
        return out.count == expected ? out : nil
    }
}
