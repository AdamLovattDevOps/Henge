import Foundation

/// m4 fork: engine settings, without a rebuild. Defaults below; Documents/henge-env.txt
/// (KEY=VALUE per line, # comments) overrides them, and can be written over USB between runs.
enum EngineTuning {
    static let defaults: [String: String] = [
        "HV_HZ": "120",        // the refresh rate macOS is told its display has
        "HV_WFI_SPIN": "0",    // microseconds an idle CPU watches for work before sleeping
        "HV_STATS": "10",
        "HV_WHITE_GAIN": "7.19", // macOS puts SDR white at 0.139 linear on this display; 1/0.139      // seconds between engine stats lines in the log; 0 = off
    ]

    static func apply() {
        var env = defaults
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        // GPU command trace: on while Documents/trace-on exists (it is checked every frame, so it can be toggled live).
        env["HV_PVG_TRACE_FILE"] = docs.appendingPathComponent("trace-on").path
        let url = docs.appendingPathComponent("henge-env.txt")
        if let text = try? String(contentsOf: url, encoding: .utf8) {
            for raw in text.split(whereSeparator: \.isNewline) {
                let line = raw.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init)?
                    .trimmingCharacters(in: .whitespaces) ?? ""
                guard let eq = line.firstIndex(of: "="), eq != line.startIndex else { continue }
                env[String(line[..<eq]).trimmingCharacters(in: .whitespaces)] = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
            }
        }
        for (k, v) in env.sorted(by: { $0.key < $1.key }) {
            setenv(k, v, 1)
            HengeLog.log("tuning", "\(k)=\(v)")
        }
    }
}
